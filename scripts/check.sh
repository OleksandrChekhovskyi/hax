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
if [ "$BUILD_DIR" = build ]; then
    build_suffix=
else
    build_suffix=" ($BUILD_DIR)"
fi

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

    setup_log=$(mktemp)
    trap 'rm -f "$setup_log"' 0
    if ! meson setup "$BUILD_DIR" "$@" >"$setup_log" 2>&1; then
        cat "$setup_log" >&2
        exit 1
    fi
    rm -f "$setup_log"
    trap - 0
    printf "setup OK%s\n" "$build_suffix"
}

build_project() {
    setup_build_dir
    ninja -C "$BUILD_DIR" --quiet
    printf "build OK%s\n" "$build_suffix"
}

lint_sources() {
    find src tests -type f \( -name '*.c' -o -name '*.h' \) \
        -exec clang-format --dry-run --Werror {} +

    # Tests use t_tempdir() so temporary directories are removed on early exits.
    if grep -rn --include='*.c' --include='*.h' 'mkdtemp' tests |
        grep -v '^tests/harness\.h:'; then
        printf '%s\n' \
            'error: raw mkdtemp in tests; use t_tempdir() from tests/harness.h' >&2
        exit 1
    fi
    printf '%s\n' 'lint OK'
}

case ${1:-} in
build)
    build_project
    ;;
test)
    shift
    build_project
    meson test -C "$BUILD_DIR" --no-rebuild -q --print-errorlogs "$@"
    ;;
lint)
    lint_sources
    ;;
*)
    printf '%s\n' 'usage: scripts/check.sh build|test|lint [test name...]' >&2
    exit 2
    ;;
esac
