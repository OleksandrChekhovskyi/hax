#!/bin/sh
# Quiet build/test/lint wrapper for humans, editors, and coding agents.
#
# Success prints a compact confirmation (a line per phase, plus meson's
# test summary); failures pass through untouched (compiler diagnostics,
# failing-test logs). BUILD_DIR selects the meson
# build directory (default: build). `build`, `build-asan`, and `build-tsan`
# are set up automatically when missing; any other directory must already
# exist.
#
# Usage:
#   scripts/check.sh build              # quiet build
#   scripts/check.sh test [name...]     # quiet build, then all/named tests
#   scripts/check.sh lint               # clang-format check over src/ and tests/
#
#   BUILD_DIR=build-asan scripts/check.sh test

set -eu

cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build}

# Name the build dir in messages only when it isn't the default one.
if [ "$BUILD_DIR" = build ]; then
    where=""
else
    where=" ($BUILD_DIR)"
fi

setup() {
    [ -d "$BUILD_DIR" ] && return 0
    case $BUILD_DIR in
    build)
        set -- ;;
    build-asan)
        set -- -Db_sanitize=address,undefined ;;
    build-tsan)
        set -- -Db_sanitize=thread ;;
    *)
        echo "error: build dir '$BUILD_DIR' does not exist;" \
            "run: meson setup $BUILD_DIR <options>" >&2
        exit 1 ;;
    esac
    log=$(mktemp)
    if ! meson setup "$BUILD_DIR" "$@" >"$log" 2>&1; then
        cat "$log" >&2
        rm -f "$log"
        exit 1
    fi
    rm -f "$log"
    echo "setup OK$where"
}

build() {
    setup
    ninja -C "$BUILD_DIR" --quiet
    echo "build OK$where"
}

case ${1:-} in
build)
    build
    ;;
test)
    shift
    build
    meson test -C "$BUILD_DIR" --no-rebuild -q --print-errorlogs "$@"
    ;;
lint)
    find src tests -type f \( -name '*.c' -o -name '*.h' \) -print0 |
        xargs -0 clang-format --dry-run --Werror
    # Raw mkdtemp in tests leaks dirs under /tmp when a test fails or
    # forgets cleanup; t_tempdir() (tests/harness.h) removes them at exit.
    if grep -rn 'mkdtemp' tests --include='*.c' --include='*.h' |
        grep -v '^tests/harness\.h:'; then
        echo "error: raw mkdtemp in tests; use t_tempdir() from tests/harness.h" >&2
        exit 1
    fi
    echo "lint OK"
    ;;
*)
    echo "usage: scripts/check.sh build|test|lint [test name...]" >&2
    exit 2
    ;;
esac
