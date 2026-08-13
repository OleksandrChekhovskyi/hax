#!/usr/bin/env python3
"""Point the AUR package at a published hax release.

Usage: [AUR_SSH_KEY=...] scripts/maint/bump_aur.py VERSION   (e.g. 0.4.0 — no leading "v")

AUR_SSH_KEY is a private key registered on the AUR account; without it the push falls
back to whatever the ambient SSH agent or ~/.ssh offers, which is how a maintainer runs
this by hand. AUR keys are account-scoped — the AUR has no per-package deploy key — so
the key in use can write every package the account maintains.

Requires makepkg, which regenerates .SRCINFO. The AUR rejects a push whose .SRCINFO
disagrees with its PKGBUILD, so it cannot be hand-edited or skipped.

Exit status: 0 bumped or already current, 2 usage, environment, or bump error.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from release_bump import (
    check_not_downgrade,
    commit_and_push,
    die,
    extract_once,
    fetch_sha256,
    git,
    parse_version,
    source_url,
    substitute_once,
)

# Reading needs no credential, so the recipe is prepared over the anonymous remote and
# only the push uses the key. See stage() for why that separation matters.
READ_REMOTE = "https://aur.archlinux.org/hax.git"
PUSH_REMOTE = "ssh://aur@aur.archlinux.org/hax.git"
RECIPE = ("PKGBUILD", ".SRCINFO")


def write_srcinfo(repo: Path) -> None:
    proc = subprocess.run(
        ("makepkg", "--printsrcinfo"), cwd=repo, capture_output=True, text=True
    )
    if proc.returncode != 0:
        die(f"makepkg --printsrcinfo failed: {proc.stderr.strip()}")
    (repo / ".SRCINFO").write_text(proc.stdout)


def check_srcinfo(repo: Path, url: str, sha256: str) -> None:
    """Confirm the generated .SRCINFO points at the artifact whose checksum was recorded.
    PKGBUILD builds its source from $pkgver today, but nothing forces it to: an explicit
    URL left behind by an edit would pair the old tarball with the new checksum, and only
    .SRCINFO shows the expression already expanded."""
    srcinfo = (repo / ".SRCINFO").read_text()
    fields = [line.strip().split(" = ", 1) for line in srcinfo.splitlines() if " = " in line]
    # The bump overwrites the whole sha256sums array with one entry, so a recipe that has
    # grown a patch or a second source would be published with checksums that no longer
    # line up. Adding one means teaching this script to keep the others.
    for key, count in (("source", 1), ("sha256sums", 1)):
        found = sum(1 for name, _ in fields if name == key)
        if found != count:
            die(f"expected {count} {key} in .SRCINFO, found {found}; this bump rewrites "
                "the whole array and cannot carry extra sources")
    if f"::{url}" not in srcinfo:
        die(f"generated .SRCINFO does not source {url}; check the PKGBUILD source line")
    if sha256 not in srcinfo:
        die(f"generated .SRCINFO does not carry {sha256}; check the PKGBUILD sums line")


def stage(workdir: Path, version: str, url: str, sha256: str) -> tuple[Path, str, str]:
    """Prepare the recipe over the anonymous remote, which needs no credential. Returns the
    staged repository, the resulting pkgrel, and the upstream commit it was based on."""
    repo = workdir / "staged"
    git("clone", READ_REMOTE, str(repo))
    base = git("rev-parse", "HEAD", cwd=repo)

    pkgbuild = repo / "PKGBUILD"
    text = pkgbuild.read_text()

    current = extract_once(text, r"^pkgver=(.*)$", "pkgver line")
    check_not_downgrade(current, version)

    text = substitute_once(text, r"^pkgver=.*$", f"pkgver={version}", "pkgver line")
    text = substitute_once(
        text, r"^sha256sums=.*$", f'sha256sums=("{sha256}")', "sha256sums line"
    )
    # pkgrel counts packaging revisions within one upstream version, so a new version
    # restarts it. Re-running the same version keeps the current value: resetting it then
    # would republish an older package as if it were newer.
    if current != version:
        text = substitute_once(text, r"^pkgrel=.*$", "pkgrel=1", "pkgrel line")
    pkgrel = extract_once(text, r"^pkgrel=(.*)$", "pkgrel line")

    pkgbuild.write_text(text)
    write_srcinfo(repo)
    check_srcinfo(repo, url, sha256)
    return repo, pkgrel, base


def publish(workdir: Path, staged: Path, message: str, key: str, base: str) -> bool:
    """Push the staged recipe from a repository the staging phase never touched, so a
    tampered PKGBUILD cannot have planted a repo-local hook or ssh command for this step
    to run with the key. Global config stays reachable — see main()."""
    env = {}
    if key:
        keyfile = workdir / "aur_key"
        keyfile.write_text(key if key.endswith("\n") else key + "\n")
        keyfile.chmod(0o600)
        env["GIT_SSH_COMMAND"] = f"ssh -i {keyfile} -o IdentitiesOnly=yes"

    repo = workdir / "publish"
    git("clone", PUSH_REMOTE, str(repo), env=env or None)
    # The staged recipe replaces this clone wholesale, so the checks it passed only hold
    # if the package has not moved since. Anything landing in between — a hand-pushed
    # version, a pkgrel rebuild — would otherwise be reverted by a clean fast-forward.
    if git("rev-parse", "HEAD", cwd=repo) != base:
        die("the AUR package changed while this bump was preparing; re-run it")

    for name in RECIPE:
        shutil.copyfile(staged / name, repo / name)
    return commit_and_push(repo, message, env=env or None)


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: scripts/maint/bump_aur.py VERSION")
    version = sys.argv[1]
    parse_version(version)  # reject a prerelease before any network or clone work

    # Dropped before makepkg runs, since a non-login su would otherwise hand the PKGBUILD
    # this value outright. It is not a boundary: makepkg executes the recipe under this
    # same uid, which can still reach the key through /proc, through the file publish()
    # writes, or by diverting the push with global git or ssh config. Closing that off
    # would take a separate uid per phase, which is only worth it once this key opens more
    # than the one package a tampered recipe would already have come from.
    key = os.environ.pop("AUR_SSH_KEY", "")

    # makepkg refuses to run as root, including for --printsrcinfo.
    if os.geteuid() == 0:
        die("makepkg refuses to run as root; run this as an unprivileged user")

    url = source_url(version)
    sha256 = fetch_sha256(url)

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        staged, pkgrel, base = stage(workdir, version, url, sha256)
        # Arch's devtools convention. pkgrel is part of a package's identity, so naming
        # it keeps a packaging-only rebuild distinct from a version bump.
        pushed = publish(workdir, staged, f"upgpkg: hax {version}-{pkgrel}", key, base)

    if pushed:
        print(f"AUR package bumped to {version}-{pkgrel}")
    else:
        print(f"AUR package is already at {version}-{pkgrel}; nothing to publish")


if __name__ == "__main__":
    main()
