#!/usr/bin/env python3
"""Point the Homebrew tap's formula at a published hax release.

Usage: GH_TOKEN=... scripts/maint/bump_homebrew.py VERSION   (e.g. 0.4.0 — no leading "v")

GH_TOKEN needs push access to the tap repository; the default workflow token only
reaches the hax repository.

Exit status: 0 bumped or already current, 2 usage, environment, or bump error.
"""

from __future__ import annotations

import os
import re
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
    hide_secret,
    parse_version,
    source_url,
    substitute_once,
)

TAP = "OleksandrChekhovskyi/homebrew-hax"
FORMULA = "Formula/hax.rb"


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: GH_TOKEN=... scripts/maint/bump_homebrew.py VERSION")
    version = sys.argv[1]
    parse_version(version)  # reject a prerelease before any network or clone work

    token = os.environ.get("GH_TOKEN")
    if not token:
        die("GH_TOKEN is not set")
    hide_secret(token)

    url = source_url(version)
    sha256 = fetch_sha256(url)

    with tempfile.TemporaryDirectory() as workdir:
        tap = Path(workdir) / "tap"
        remote = f"https://x-access-token:{token}@github.com/{TAP}.git"
        git("clone", "--depth", "1", remote, str(tap))

        formula = tap / FORMULA
        text = formula.read_text()

        current = extract_once(text, r"^  url .*/hax-(.*)\.tar\.xz\"$", "url line")
        check_not_downgrade(current, version)

        text = substitute_once(text, r"^  url .*$", f'  url "{url}"', "url line")
        text = substitute_once(text, r"^  sha256 .*$", f'  sha256 "{sha256}"', "sha256 line")
        # A bottle block holds checksums of the previous version's bottles; drop it
        # until they are rebuilt. Absent on a formula that has never been bottled.
        text = re.sub(r"^  bottle do\n(?:.*\n)*?  end\n", "", text, flags=re.MULTILINE)

        formula.write_text(text)
        pushed = commit_and_push(tap, f"hax {version}")

    if pushed:
        print(f"tap bumped to {version}")
    else:
        print(f"tap is already at {version}; nothing to publish")


if __name__ == "__main__":
    main()
