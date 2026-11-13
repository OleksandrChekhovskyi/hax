#!/bin/sh
# Build the macOS release binary. It links only libraries every macOS ships: libSystem and the
# system libcurl, which verifies TLS against the Keychain. jansson builds from a pinned release
# tarball and links statically. The tested, stripped binary lands in BUILD_DIR (default
# build-macos).
# Usage: [BUILD_DIR=dir] scripts/build_macos.sh

set -eu

cd "$(dirname "$0")/.."

if [ $# -ne 0 ]; then
    printf '%s\n' 'usage: [BUILD_DIR=dir] scripts/build_macos.sh' >&2
    exit 2
fi
if [ "$(uname -s)" != Darwin ]; then
    printf '%s\n' 'error: the macOS binary builds only on macOS' >&2
    exit 1
fi

# Big Sur, the first release for Apple Silicon. The variable also reaches jansson's build, and
# the availability error keeps hax from calling anything newer without a runtime check.
MACOSX_DEPLOYMENT_TARGET=11.0
export MACOSX_DEPLOYMENT_TARGET

BUILD_DIR=${BUILD_DIR:-build-macos}
export BUILD_DIR
wipe=
[ -d "$BUILD_DIR" ] && wipe=--wipe

# Pinned; see "Version pins" in docs/releasing.md.
JANSSON_VERSION=2.15.1
JANSSON_SHA256=0c7114dc0b2d22a670724a1f95922029d7077c19dbf79a584cb8084d2f267f2f

# A static-only install: with no dylib beside it, the linker has to take the archive.
deps=$(mktemp -d)
trap 'rm -rf "$deps"' 0
url=https://github.com/akheron/jansson/releases/download/v$JANSSON_VERSION
tarball=$deps/jansson-$JANSSON_VERSION.tar.gz
curl -fsSL -o "$tarball" "$url/jansson-$JANSSON_VERSION.tar.gz"
printf '%s  %s\n' "$JANSSON_SHA256" "$tarball" | shasum -a 256 -c - >/dev/null
tar -xzf "$tarball" -C "$deps"
(
    cd "$deps/jansson-$JANSSON_VERSION"
    ./configure --quiet --prefix="$deps/prefix" --disable-shared
    make -s install
) >/dev/null
printf 'jansson %s OK\n' "$JANSSON_VERSION"

# Setup output stays on stdout: the release log's record of the artifact's options and
# dependency versions.
PKG_CONFIG_PATH=$deps/prefix/lib/pkgconfig meson setup $wipe "$BUILD_DIR" --buildtype=release \
    -Dc_args=-Werror=unguarded-availability

scripts/check.sh test

bin=$BUILD_DIR/hax
# A library from Homebrew or another prefix would be missing on users' machines.
foreign=$(otool -L "$bin" | tail -n +2 | grep -Ev '^[[:space:]]+/(usr/lib|System/Library)/' || :)
if [ -n "$foreign" ]; then
    printf 'error: %s links libraries macOS does not ship:\n%s\n' "$bin" "$foreign" >&2
    exit 1
fi

strip "$bin"
codesign --verify "$bin"
"$bin" --version

printf '%s: %s, system libraries only, stripped\n' "$bin" "$(du -h "$bin" | cut -f1)"
