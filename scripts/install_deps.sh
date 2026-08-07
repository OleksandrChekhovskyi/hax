#!/bin/sh
# Install the build/test dependencies for the current platform; `lint` additionally
# installs the lint toolchain (clang-format, clang-tidy). Serves developers and CI
# alike: package managers prompt as usual on a tty and run unattended without one.
# Usage: scripts/install_deps.sh [lint]

set -eu

usage() {
    printf '%s\n' 'usage: scripts/install_deps.sh [lint]' >&2
    exit 2
}

[ $# -le 1 ] || usage

lint=
case ${1:-} in
lint)
    lint=1
    ;;
'') ;;
*)
    usage
    ;;
esac

if [ -t 0 ]; then
    apt_yes=
    noconfirm=
else
    apt_yes=-y
    noconfirm=--noconfirm
fi

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

if [ "$(uname)" = Darwin ]; then
    brew install jansson meson ninja pkg-config ${lint:+llvm}
    exit 0
fi

. /etc/os-release

case "$ID ${ID_LIKE:-}" in
*debian* | *ubuntu*)
    as_root apt-get update
    as_root apt-get install $apt_yes --no-install-recommends \
        build-essential libcurl4-openssl-dev libjansson-dev \
        meson ninja-build pkg-config python3 \
        ${lint:+clang-format clang-tidy}
    ;;
*arch*)
    arch_pkgs="gcc make curl jansson meson ninja pkgconf python ${lint:+clang}"
    # Disposable containers need the full sync-and-upgrade (a bare -Sy install risks a
    # partial upgrade), but only on explicit opt-in from the CI workflow: `CI` alone also
    # describes self-hosted runners on real machines. Elsewhere only the listed packages
    # are installed, and syncing a stale database stays the machine owner's decision.
    if [ -n "${HAX_DEPS_UPGRADE:-}" ]; then
        # Official Arch container images strip the keyring's local master key, leaving
        # keyring upgrades unable to sign newly added packager keys (pacman hides the
        # hook failure behind exit 0). Regenerate it before touching packages.
        as_root pacman-key --init
        as_root pacman-key --populate archlinux
        as_root pacman -Syu --noconfirm --needed $arch_pkgs
    else
        as_root pacman -S --needed $noconfirm $arch_pkgs
    fi
    ;;
*alpine*)
    if [ -n "$lint" ]; then
        printf '%s\n' 'error: the lint toolchain is not supported on Alpine (see .clang-tidy)' >&2
        exit 1
    fi
    as_root apk add --no-cache \
        build-base meson samurai curl-dev jansson-dev python3
    ;;
*)
    printf "error: unsupported platform '%s'; see README.md for dependencies\n" "$ID" >&2
    exit 1
    ;;
esac
