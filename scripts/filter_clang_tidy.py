#!/usr/bin/env python3
"""Remove routine progress and summary lines from failed run-clang-tidy output."""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable, Iterator
from typing import Final

HEADING_RE: Final = re.compile(r"^Running clang-tidy (for|in) ")
SUMMARY_RE: Final = re.compile(
    r"^[0-9]+ (warnings?|errors?)( and [0-9]+ (warnings?|errors?))? generated\.$"
)
PROGRESS_RE: Final = re.compile(r"^\[[ 0-9]+/[0-9]+\]\[[0-9.]+s\] $")


def is_invocation(text: str, clang_tidy: str) -> bool:
    command = f"{clang_tidy} "
    position = text.find(command)
    if position == 0:
        return True
    return position > 0 and PROGRESS_RE.fullmatch(text[:position]) is not None


def filter_output(lines: Iterable[str], clang_tidy: str) -> Iterator[str]:
    after_summary = False
    for line in lines:
        text = line.rstrip("\r\n")
        if HEADING_RE.match(text) or is_invocation(text, clang_tidy):
            continue
        if SUMMARY_RE.fullmatch(text):
            after_summary = True
            continue
        if after_summary and not text.strip():
            continue
        after_summary = False
        yield line


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} CLANG_TIDY", file=sys.stderr)
        return 2
    sys.stdout.writelines(filter_output(sys.stdin, sys.argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
