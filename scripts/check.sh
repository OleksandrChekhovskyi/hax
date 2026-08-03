#!/bin/sh
# Build, test, and lint wrapper. Successful phases emit compact confirmations; failures emit
# diagnostics without routine runner output.
# Usage: [BUILD_DIR=dir] scripts/check.sh build|lint
#        [BUILD_DIR=dir] scripts/check.sh test [name...]

set -eu

cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build}
# Canonicalize BUILD_DIR because setup matches exact directory names and relay_log computes parent
# traversal depth from its spelling. Paths outside the repository cannot be rebased to its root.
while [ "${BUILD_DIR#./}" != "$BUILD_DIR" ]; do BUILD_DIR=${BUILD_DIR#./}; done
while [ "${BUILD_DIR%/}" != "$BUILD_DIR" ]; do BUILD_DIR=${BUILD_DIR%/}; done
case $BUILD_DIR in
"$(pwd -P)"/*)
    BUILD_DIR=${BUILD_DIR#"$(pwd -P)"/}
    ;;
esac
if [ "$BUILD_DIR" = build ]; then
    build_suffix=
else
    build_suffix=" ($BUILD_DIR)"
fi

# Homebrew's keg-only LLVM tools are not linked into PATH on macOS.
llvm_tool() {
    if command -v "$1" >/dev/null 2>&1; then
        command -v "$1"
        return
    fi
    llvm_prefix="${llvm_prefix:-$(brew --prefix llvm 2>/dev/null || true)}"
    if [ -n "$llvm_prefix" ] && [ -x "$llvm_prefix/bin/$1" ]; then
        printf '%s\n' "$llvm_prefix/bin/$1"
        return
    fi
    printf "error: %s not found; see the development tools section in README.md\n" "$1" >&2
    exit 1
}

# Meson forces colored diagnostics, while compilers report paths relative to the build directory.
# Strip ANSI codes and rebase in-repo paths so editor quickfix parsers resolve them from the root.
relay_log() {
    esc=$(printf '\033')
    case $BUILD_DIR in
    /* | ../*)
        sed "s|$esc\[[0-9;]*[mK]||g" "$captured_log"
        ;;
    *)
        up_prefix=$(printf '%s/' "$BUILD_DIR" | sed 's|[^/][^/]*|..|g')
        sed -e "s|$esc\[[0-9;]*[mK]||g" \
            -e "s|^$(printf '%s' "$up_prefix" | sed 's|\.|\\.|g')||" "$captured_log"
        ;;
    esac
}

run_captured() {
    captured_log=$(mktemp)
    trap 'rm -f "$captured_log"' 0
    if ! "$@" >"$captured_log" 2>&1; then
        relay_log >&2
        exit 1
    fi
}

run_clang_tidy_captured() {
    captured_log=$(mktemp)
    compact_log=$captured_log.compact
    trap 'rm -f "$captured_log" "$compact_log"' 0
    if ! "$@" >"$captured_log" 2>&1; then
        python3 scripts/filter_clang_tidy.py "$clang_tidy" <"$captured_log" >"$compact_log"
        mv "$compact_log" "$captured_log"
        relay_log >&2
        exit 1
    fi
}

drop_captured() {
    rm -f "$captured_log"
    trap - 0
}

setup_build_dir() {
    [ -d "$BUILD_DIR" ] && return
    setup_quiet=${1:-}

    case $BUILD_DIR in
    build)
        set --
        ;;
    build-asan)
        set -- -Db_sanitize=address,undefined
        ;;
    build-tsan)
        set -- -Db_sanitize=thread
        ;;
    *)
        printf "error: build dir '%s' does not exist; run: meson setup %s <options>\n" \
            "$BUILD_DIR" "$BUILD_DIR" >&2
        exit 1
        ;;
    esac

    run_captured meson setup "$BUILD_DIR" "$@"
    drop_captured
    [ "$setup_quiet" = quiet ] || printf "setup OK%s\n" "$build_suffix"
}

build_project() {
    setup_build_dir
    run_captured ninja -C "$BUILD_DIR" --quiet
    drop_captured
    printf "build OK%s\n" "$build_suffix"
}

lint_sources() {
    clang_format=$(llvm_tool clang-format)
    clang_tidy=$(llvm_tool clang-tidy)
    run_clang_tidy=$(llvm_tool run-clang-tidy)

    # Apple's cc resolves the SDK implicitly, so the compile database lacks -isysroot and
    # Homebrew's clang-tidy cannot find system headers. Clang's Darwin driver honors SDKROOT.
    if [ "$(uname)" = Darwin ] && [ -z "${SDKROOT:-}" ]; then
        SDKROOT=$(xcrun --show-sdk-path)
        export SDKROOT
    fi

    find src tests -type f \( -name '*.c' -o -name '*.h' \) \
        -exec "$clang_format" --dry-run --Werror --ferror-limit=1 {} +
    python3 scripts/lint_style.py

    # Regenerate the compile database before clang-tidy; otherwise translation units added since
    # the last Meson setup are silently skipped.
    setup_build_dir quiet
    run_captured ninja -C "$BUILD_DIR" --quiet build.ninja
    drop_captured
    run_clang_tidy_captured "$run_clang_tidy" -clang-tidy-binary "$clang_tidy" -quiet \
        -p "$BUILD_DIR"
    drop_captured
    printf '%s\n' 'lint OK'
}

case ${1:-} in
build)
    build_project
    ;;
test)
    shift
    build_project
    run_captured meson test -C "$BUILD_DIR" --no-rebuild -q --print-errorlogs "$@"
    relay_log
    drop_captured
    ;;
lint)
    lint_sources
    ;;
*)
    printf '%s\n' 'usage: scripts/check.sh build|test|lint [test name...]' >&2
    exit 2
    ;;
esac
