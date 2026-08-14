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
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    elif command -v doas >/dev/null 2>&1; then
        # OpenBSD ships doas in base and has no sudo unless it is installed.
        doas "$@"
    else
        printf '%s\n' 'error: need root; install sudo or doas, or run as root' >&2
        exit 1
    fi
}

# The lint toolchain is wired up only where CI exercises the clang-tidy range
# .clang-tidy promises to stay clean for. Elsewhere, refuse rather than install
# a version whose findings the project does not treat as failures.
reject_lint() {
    if [ -n "$lint" ]; then
        printf 'error: the lint toolchain is not supported on %s (see .clang-tidy)\n' "$1" >&2
        exit 1
    fi
}

# The BSDs are selected by uname rather than os-release: OpenBSD ships no
# such file, and reaching the sourcing below would abort the script outright.
case "$(uname)" in
Darwin)
    brew install jansson meson ninja pkg-config ${extras:+fzf} ${lint:+llvm}
    exit 0
    ;;
FreeBSD)
    reject_lint FreeBSD
    # clang and make come from the base system, and pkgconf provides
    # pkg-config. python3 is explicit because meson depends on a versioned
    # python package that need not provide the unversioned command.
    as_root pkg install $assume_yes curl jansson meson ninja pkgconf \
        python3 ${extras:+fzf}
    exit 0
    ;;
OpenBSD)
    reject_lint OpenBSD
    # clang, make and pkg-config all come from the base system here, and meson
    # brings a python3 that the e2e tests can use.
    as_root pkg_add -I curl jansson meson ninja ${extras:+fzf}
    exit 0
    ;;
esac

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
    reject_lint Alpine
    as_root apk add --no-cache \
        build-base meson samurai curl-dev jansson-dev python3 ${extras:+fzf}
    ;;
*)
    printf "error: unsupported platform '%s'; see README.md for dependencies\n" "$ID" >&2
    exit 1
    ;;
esac
