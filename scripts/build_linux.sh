#!/bin/sh
# Build the fully static Linux release binary. Requires musl (glibc cannot link NSS statically),
# hence Alpine: runs there directly, elsewhere re-executes itself in an alpine container via
# podman. ARCH selects the container CPU; a foreign one emulates via qemu binfmt, slowly. The
# tested, stripped binary lands in BUILD_DIR (default build-linux-<arch>) and in artifact-path
# when given.
# Usage: [ARCH=cpu] [BUILD_DIR=dir] scripts/build_linux.sh [artifact-path]

set -eu

cd "$(dirname "$0")/.."

usage() {
    printf '%s\n' 'usage: [ARCH=cpu] [BUILD_DIR=dir] scripts/build_linux.sh [artifact-path]' >&2
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
        docker.io/library/alpine:latest sh scripts/build_linux.sh "$@"
    # Never report success unless the binary reached the host — the last resort against an
    # escape the resolution above missed.
    out=${artifact:-${BUILD_DIR:-build-linux-$arch}/hax}
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

# The binary is tested before it ships, so it needs what a CI run installs; errors still reach
# stderr.
scripts/install_deps.sh ci >/dev/null
# Every library in the static link must be present as a static archive. cmake builds libcurl;
# git is for the version stamp.
as_root apk add --no-cache -q \
    git cmake jansson-static mbedtls-dev mbedtls-static zlib-dev zlib-static
printf '%s\n' 'deps OK'

# Pinned; see "Version pins" in docs/releasing.md.
CURL_VERSION=8.23.0
CURL_SHA256=e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49

# Alpine's libcurl links OpenSSL and every optional protocol library, quadrupling the binary,
# so libcurl builds from a pinned release with only what hax uses: HTTP(S) over mbedTLS, gzip,
# proxies, and the threaded resolver. It bakes in no CA store path: one valid on Alpine is
# missing on other distributions, and hax probes the running system's store instead.
deps=$(mktemp -d)
trap 'rm -rf "$deps"' 0
tarball=$deps/curl-$CURL_VERSION.tar.gz
wget -q -O "$tarball" "https://curl.se/download/curl-$CURL_VERSION.tar.gz"
printf '%s  %s\n' "$CURL_SHA256" "$tarball" | sha256sum -c - >/dev/null
tar -xzf "$tarball" -C "$deps"
cmake -S "$deps/curl-$CURL_VERSION" -B "$deps/build" -G Ninja --log-level=WARNING \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$deps/prefix" \
    -DCMAKE_C_FLAGS='-ffunction-sections -fdata-sections' \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
    -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_ZLIB=ON \
    -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_GSSAPI=OFF -DENABLE_ARES=OFF \
    -DCURL_DISABLE_ALTSVC=ON -DCURL_DISABLE_AWS=ON -DCURL_DISABLE_COOKIES=ON \
    -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_DOH=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON \
    -DCURL_DISABLE_FORM_API=ON -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_HSTS=ON \
    -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_IPFS=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_MIME=ON \
    -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_NETRC=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_RTSP=ON \
    -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON \
    -DCURL_DISABLE_WEBSOCKETS=ON >/dev/null
cmake --build "$deps/build" >/dev/null
cmake --install "$deps/build" >/dev/null
printf 'libcurl %s OK\n' "$CURL_VERSION"

# A bind-mounted checkout may be owned by another uid; git then refuses to run and the
# version stamp silently falls back to the project version.
if [ -e .git ] && ! git rev-parse --git-dir >/dev/null 2>&1; then
    git config --global --add safe.directory "$(pwd -P)"
fi

# Arch-suffixed default so emulated foreign-arch builds do not clobber the native one;
# --wipe because the toolchain can differ between container runs.
BUILD_DIR=${BUILD_DIR:-build-linux-$(uname -m)}
export BUILD_DIR
wipe=
[ -d "$BUILD_DIR" ] && wipe=--wipe

# Packed relative relocations reduce a static PIE's relocation table to a fraction, and
# --gc-sections drops the unused parts of libcurl's per-function sections. Setup output stays
# on stdout: the release log's record of the artifact's options and dependency versions.
PKG_CONFIG_PATH=$deps/prefix/lib/pkgconfig meson setup $wipe "$BUILD_DIR" --buildtype=release \
    -Dprefer_static=true -Dc_link_args='-static -Wl,-z,pack-relative-relocs -Wl,--gc-sections'

scripts/check.sh test

bin=$BUILD_DIR/hax
if readelf --program-headers "$bin" | grep -q INTERP ||
    readelf --dynamic "$bin" 2>/dev/null | grep -q NEEDED; then
    printf 'error: %s is not fully static\n' "$bin" >&2
    exit 1
fi
# One archive object without a .note.GNU-stack marker makes the linker fall back to an
# executable stack for the whole binary. --wide keeps the flags on the GNU_STACK row.
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
