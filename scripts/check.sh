#!/bin/sh
# Build, test, and lint wrapper for terminal use and editor integration.
# Successful phases print compact confirmations; failures preserve full diagnostics.
# BUILD_DIR defaults to build. build, build-asan, and build-tsan are created when
# missing; any other build directory must already exist.
#
# Usage:
#   scripts/check.sh build
#   scripts/check.sh test [name...]
#   scripts/check.sh lint
#   BUILD_DIR=build-asan scripts/check.sh test

set -eu

cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build}
# Canonicalize BUILD_DIR so the auto-setup name match and relay_log's ../
# depth both see one spelling: strip ./ and trailing /, and turn an absolute
# path inside the repo into the equivalent relative one. A build dir outside
# the repo stays absolute; relay_log then leaves paths as emitted.
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

# Resolve an LLVM tool: PATH first, then Homebrew's keg-only llvm, which macOS
# does not link into PATH (clang-format and clang-tidy both live there).
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

# Run "$@" with output captured to $captured_log; on failure relay the log and
# exit. Meson forces -fdiagnostics-color=always and compilers report file
# positions relative to the build dir (../src/foo.c, one ../ per path
# component of $BUILD_DIR); relay_log strips the color codes and rewrites the
# paths relative to the repo root so that quickfix parsers (nvim :make) and
# anyone else reading from here resolve them.
relay_log() {
    esc=$(printf '\033')
    case $BUILD_DIR in
    /* | ../*)
        # Build dir outside the repo: no meaningful rewrite exists.
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

drop_captured() {
    rm -f "$captured_log"
    trap - 0
}

setup_build_dir() {
    [ -d "$BUILD_DIR" ] && return

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
    printf "setup OK%s\n" "$build_suffix"
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

    # Apple's cc resolves the SDK implicitly, so the compile database carries
    # no -isysroot and Homebrew's clang-tidy cannot find system headers.
    # SDKROOT is honored by clang's Darwin driver and fills that gap.
    if [ "$(uname)" = Darwin ] && [ -z "${SDKROOT:-}" ]; then
        SDKROOT=$(xcrun --show-sdk-path)
        export SDKROOT
    fi

    find src tests -type f \( -name '*.c' -o -name '*.h' \) \
        -exec "$clang_format" --dry-run --Werror {} +
    python3 scripts/lint_style.py

    # clang-tidy needs the compile database from a configured build dir, and
    # the database must be regenerated when meson.build changed, or new
    # translation units silently skip analysis.
    setup_build_dir
    run_captured ninja -C "$BUILD_DIR" build.ninja
    drop_captured
    run_captured "$run_clang_tidy" -clang-tidy-binary "$clang_tidy" -quiet -p "$BUILD_DIR"
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
