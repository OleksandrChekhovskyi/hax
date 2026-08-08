#!/bin/sh
# Build the fully static Linux release binary. Requires musl (glibc cannot link NSS statically),
# hence Alpine: runs there directly, elsewhere re-executes itself in an alpine container via
# podman. ARCH selects the container CPU; a foreign one emulates via qemu binfmt, slowly. The
# tested, stripped binary lands in BUILD_DIR (default build-static-<arch>) and in artifact-path
# when given.
# Usage: [ARCH=cpu] [BUILD_DIR=dir] scripts/build_static.sh [artifact-path]

set -eu

cd "$(dirname "$0")/.."

usage() {
    printf '%s\n' 'usage: [ARCH=cpu] [BUILD_DIR=dir] scripts/build_static.sh [artifact-path]' >&2
    exit 2
}

[ $# -le 1 ] || usage
artifact=${1:-}

if [ ! -f /etc/alpine-release ]; then
    if ! command -v podman >/dev/null 2>&1; then
        printf '%s\n' 'error: building outside Alpine requires podman' >&2
        exit 1
    fi
    # Only the repository is bind-mounted: a path escaping it via /, .., or a symlink would be
    # written to the container filesystem and silently discarded with the container. Resolve
    # each path's deepest existing ancestor and require it under the repository root.
    root=$(pwd -P)
    for path in "$artifact" "${BUILD_DIR:-}"; do
        [ -n "$path" ] || continue
        # A final file symlink is followed by cp inside the container, so the host target
        # stays stale yet satisfies the output check. Directory symlinks resolve below.
        if [ -L "$path" ] && [ ! -d "$path" ]; then
            printf "error: '%s' is a symlink; podman builds resolve it in the container\n" \
                "$path" >&2
            exit 1
        fi
        anchor=$path
        while [ ! -d "$anchor" ]; do
            anchor=$(dirname "$anchor")
        done
        case $(cd "$anchor" && pwd -P) in
        "$root" | "$root"/*) ;;
        *)
            printf "error: '%s' leaves the repository, which a podman build cannot write\n" \
                "$path" >&2
            exit 1
            ;;
        esac
    done
    case ${ARCH:-$(uname -m)} in
    x86_64 | amd64)
        platform=amd64
        arch=x86_64
        ;;
    aarch64 | arm64)
        platform=arm64
        arch=aarch64
        ;;
    *)
        printf "error: unsupported ARCH '%s'\n" "${ARCH:-$(uname -m)}" >&2
        exit 1
        ;;
    esac
    # Pin --arch: a pulled foreign image variant retags the local alpine:latest, and an
    # unqualified run would then silently keep emulating that foreign CPU.
    podman run --rm --arch "$platform" -v "$(pwd -P):/src" -w /src \
        ${BUILD_DIR:+--env "BUILD_DIR=$BUILD_DIR"} \
        docker.io/library/alpine:latest sh scripts/build_static.sh "$@"
    # Never report success unless the binary reached the host — the last resort against an
    # escape the resolution above missed.
    out=${artifact:-${BUILD_DIR:-build-static-$arch}/hax}
    if [ ! -f "$out" ]; then
        printf "error: '%s' was not written to the host; the path escapes the repository\n" \
            "$out" >&2
        exit 1
    fi
    exit 0
fi

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

# The static link resolves every library in libcurl.pc's Libs.private, so each one must be
# present as a static archive (c-ares ships its .a in the dev package). git is for the
# version stamp.
as_root apk add --no-cache -q \
    build-base git meson samurai python3 \
    curl-dev curl-static jansson-dev jansson-static \
    brotli-static c-ares-dev libidn2-static libpsl-static libunistring-static \
    nghttp2-static openssl-libs-static zlib-static zstd-static
printf '%s\n' 'deps OK'

# A bind-mounted checkout may be owned by another uid; git then refuses to run and the
# version stamp silently falls back to the project version.
if [ -e .git ] && ! git rev-parse --git-dir >/dev/null 2>&1; then
    git config --global --add safe.directory "$(pwd -P)"
fi

# Arch-suffixed default so emulated foreign-arch builds do not clobber the native one;
# --wipe because the toolchain can differ between container runs.
BUILD_DIR=${BUILD_DIR:-build-static-$(uname -m)}
export BUILD_DIR
wipe=
[ -d "$BUILD_DIR" ] && wipe=--wipe

# Alpine ships some static archives as slim LTO bytecode (brotli, libpsl), so every link runs
# LTO recompilation; -flto=2 caps its workers, which otherwise default to nproc for each of
# ninja's already-parallel links. Setup output stays on stdout: the release log's record of
# the artifact's options and dependency versions.
meson setup $wipe "$BUILD_DIR" --buildtype=release \
    -Dprefer_static=true -Dc_link_args='-static -flto=2'

# lto-wrapper cannot merge the archives' mismatched -Xassembler options and warns per link;
# the only consequential drop is --noexecstack, covered by the stack check below, so filter
# exactly that message. Captured, not piped: a failure must keep its exit status and full log.
log=$(mktemp)
trap 'rm -f "$log"' 0
if ! scripts/check.sh test >"$log" 2>&1; then
    cat "$log" >&2
    exit 1
fi
grep -v "^lto-wrapper: warning: Extra option to '-Xassembler'" "$log" || :

bin=$BUILD_DIR/hax
if readelf --program-headers "$bin" | grep -q INTERP ||
    readelf --dynamic "$bin" 2>/dev/null | grep -q NEEDED; then
    printf 'error: %s is not fully static\n' "$bin" >&2
    exit 1
fi
# The filtered lto-wrapper warnings mean --noexecstack relied on the assembler emitting the
# marker itself; verify it did. --wide keeps the flags on the GNU_STACK row.
if ! readelf --wide --program-headers "$bin" | grep -q 'GNU_STACK.*RW '; then
    printf 'error: %s has an executable or unmarked stack\n' "$bin" >&2
    exit 1
fi

strip "$bin"
"$bin" --version

if [ -n "$artifact" ]; then
    mkdir -p "$(dirname "$artifact")"
    cp "$bin" "$artifact"
    bin=$artifact
fi
printf '%s: %s, statically linked, stripped\n' "$bin" "$(du -h "$bin" | cut -f1)"
