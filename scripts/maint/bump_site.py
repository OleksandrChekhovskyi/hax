#!/usr/bin/env python3
"""Point the usehax.dev manual at a published hax release.

Usage: GH_TOKEN=... scripts/maint/bump_site.py VERSION   (e.g. 0.4.0 — no leading "v")

GH_TOKEN needs push access to the site repository; the default workflow token only
reaches the hax repository.

The site's tools/make-man.py renders the manual from a hax build of the release, so this
needs hax's build dependencies: meson, ninja, a C compiler, and the libcurl and jansson
headers.

Exit status: 0 bumped or already current, 2 usage, environment, or bump error.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

from release_bump import (
    check_not_downgrade,
    commit_and_push,
    die,
    git,
    hide_secret,
    parse_version,
)

SITE = "OleksandrChekhovskyi/usehax.dev"
PIN = "tools/hax-version"


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: GH_TOKEN=... scripts/maint/bump_site.py VERSION")
    version = sys.argv[1]
    parse_version(version)  # reject a prerelease before any network or clone work

    # Rendering runs the site's script and the release's build, neither of which needs
    # the token, so it stays out of their environment and out of the clone until the push.
    token = os.environ.pop("GH_TOKEN", "")
    if not token:
        die("GH_TOKEN is not set")
    hide_secret(token)

    with tempfile.TemporaryDirectory() as workdir:
        site = Path(workdir) / "site"
        git("clone", "--depth", "1", f"https://github.com/{SITE}.git", str(site))

        try:
            current = (site / PIN).read_text().strip()
        except OSError as err:
            die(f"cannot read the site's {PIN}: {err}")
        check_not_downgrade(current, version)

        proc = subprocess.run(
            (sys.executable, "tools/make-man.py", version),
            cwd=site,
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            die(f"make-man.py failed: {proc.stderr.strip() or proc.stdout.strip()}")

        # A release can add or drop man pages, which commit -a alone would not pick up.
        git("add", "--all", cwd=site)
        remote = f"https://x-access-token:{token}@github.com/{SITE}.git"
        git("remote", "set-url", "origin", remote, cwd=site)
        pushed = commit_and_push(site, f"Update the manual to hax {version}")

    if pushed:
        print(f"site manual bumped to {version}")
    else:
        print(f"site manual is already at {version}; nothing to publish")


if __name__ == "__main__":
    main()
