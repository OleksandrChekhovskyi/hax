#!/bin/sh
# Install the dependencies for building hax on the current platform. The default serves users
# building from source: the build/test dependencies plus optional tools hax uses when available
# (fzf, for @file completion). `ci` restricts it to the build/test dependencies; `lint`
# additionally installs the lint toolchain (clang-format, clang-tidy). Package managers prompt
# as usual on a tty and run unattended without one.
# Usage: scripts/install_deps.sh [ci] [lint]

set -eu

usage() {
    printf '%s\n' 'usage: scripts/install_deps.sh [ci] [lint]' >&2
    exit 2
}

extras=1
lint=
for arg; do
    case $arg in
    ci)
        extras=
        ;;
    lint)
        lint=1
        ;;
    *)
        usage
        ;;
    esac
done

if [ -t 0 ]; then
    assume_yes=
    noconfirm=
else
    assume_yes=-y
    noconfirm=--noconfirm
fi

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v doas >/dev/null 2>&1; then
        doas "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        echo "error: need root, doas, or sudo" >&2
        return 1
    fi
}

if [ "$(uname)" = Darwin ]; then
    brew install jansson meson ninja pkg-config ${extras:+fzf} ${lint:+llvm}
    exit 0
fi

if [ "$(uname)" = FreeBSD ]; then
    as_root pkg install jansson meson ninja
    exit 0
fi

. /etc/os-release

case "$ID ${ID_LIKE:-}" in
*debian* | *ubuntu*)
    as_root apt-get update
    as_root apt-get install $assume_yes --no-install-recommends \
        build-essential libcurl4-openssl-dev libjansson-dev \
        meson ninja-build pkg-config python3 ${extras:+fzf} \
        ${lint:+clang-format clang-tidy}
    ;;
*fedora* | *rhel* | *centos*)
    as_root dnf install $assume_yes gcc make libcurl-devel jansson-devel \
        meson ninja-build pkgconf-pkg-config python3 ${lint:+clang-tools-extra} || {
        printf '%s\n' 'hint: RHEL-family systems may need the CRB and EPEL repositories enabled' >&2
        exit 1
    }
    # fzf lives in EPEL on RHEL-family distributions, and dnf fails whole
    # transactions on unknown names; an extra must not cost the required set.
    if [ -n "$extras" ]; then
        as_root dnf install $assume_yes fzf ||
            printf '%s\n' 'warning: fzf unavailable (EPEL not enabled?); skipping' >&2
    fi
    ;;
*suse*)
    as_root zypper install $assume_yes gcc make libcurl-devel libjansson-devel \
        meson ninja pkgconf-pkg-config python3 ${extras:+fzf} ${lint:+clang-tools}
    ;;
*arch*)
    arch_pkgs="gcc make curl jansson meson ninja pkgconf python ${extras:+fzf} ${lint:+clang}"
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
        build-base meson samurai curl-dev jansson-dev python3 ${extras:+fzf}
    ;;
*)
    printf "error: unsupported platform '%s'; see README.md for dependencies\n" "$ID" >&2
    exit 1
    ;;
esac
