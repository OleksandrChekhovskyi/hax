#!/usr/bin/env python3
"""Print the CHANGELOG.md section for one version, for use as GitHub release notes.

Usage: scripts/release_notes.py VERSION   (e.g. 0.1.0 — no leading "v")

The section is printed trimmed of leading and trailing blank lines. A missing or
empty section fails so a release cannot ship without notes:

- 0 notes printed
- 1 no section (or an empty one) for VERSION
- 2 usage error
"""

from __future__ import annotations

import sys
from pathlib import Path


def section_body(changelog: str, version: str) -> list[str]:
    lines: list[str] = []
    in_section = False
    for line in changelog.splitlines():
        if line.startswith("## "):
            in_section = f"[{version}]" in line
            continue
        if in_section:
            lines.append(line.rstrip())
    while lines and not lines[0]:
        del lines[0]
    while lines and not lines[-1]:
        del lines[-1]
    return lines


def main() -> int:
    if len(sys.argv) != 2 or not sys.argv[1]:
        print('usage: scripts/release_notes.py VERSION (e.g. 0.1.0 — no leading "v")',
              file=sys.stderr)
        return 2

    version = sys.argv[1]
    notes = section_body(Path("CHANGELOG.md").read_text(encoding="utf-8"), version)
    if not notes:
        print(f"release_notes.py: no CHANGELOG.md section for version {version}", file=sys.stderr)
        return 1
    print("\n".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
