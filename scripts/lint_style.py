#!/usr/bin/env python3
"""Project-specific style checks that clang-format cannot express.

Checked over src/ and tests/:

- Header include guards are derived from the file path: src/render/disp.h
  uses HAX_RENDER_DISP_H (the src/ prefix is dropped, tests/ is kept), with
  a matching ``#endif /* GUARD */`` as the last line. The guard opens the
  header — only comments may precede it — so it covers every declaration.
- Quote includes name files relative to an -iquote root (src/ or tests/),
  never relative to the including file's directory, never through ``..`` or
  an absolute path.
- Include lines carry no comments except machine-read lint annotations
  (IWYU pragmas, NOLINT); rationale belongs in a comment above the include.
- Block comment delimiters sit on text lines, never alone (see
  docs/code-style.md); a lone ``*/`` merges into the last text line, rewrap
  the comment if that would overflow the column limit.
- Tests use t_tempdir() from tests/harness.h instead of raw mkdtemp so
  scratch directories are removed on early exits.

Prints findings as file:line: message and exits non-zero if any are found.
"""

from __future__ import annotations

import os
import re
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Final, NamedTuple

ROOTS: Final = (Path("src"), Path("tests"))
HARNESS: Final = Path("tests/harness.h")

INCLUDE_RE: Final = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
INCLUDE_DIRECTIVE_RE: Final = re.compile(r"^\s*#\s*include\b")
# The one accepted spelling; anything else (comments between tokens, macro
# arguments, missing space) evades the path and comment checks below.
INCLUDE_STRICT_RE: Final = re.compile(r'^#include (<[^<>"]+>|"[^"]+")')
INCLUDE_TRAIL_RE: Final = re.compile(r'^\s*#\s*include\s+[<"][^<>"]+[>"][ \t]*(\S.*)$')
LONE_DELIMITER_RE: Final = re.compile(r"^\s*(/\*\*?|\*/)\s*$")
GUARD_RE: Final = re.compile(r"^#ifndef (\w+)\n#define \1\n", re.MULTILINE)
ANCHORED_GUARD_RE: Final = re.compile(
    r"\A(?:(?:/\*.*?\*/|//[^\n]*)\s*)*#ifndef (\w+)\n#define \1\n", re.DOTALL
)


class Finding(NamedTuple):
    path: Path
    line: int
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


def expected_guard(path: Path) -> str:
    rel = path.relative_to("src") if path.is_relative_to("src") else path
    return "HAX_" + re.sub(r"[^A-Za-z0-9]", "_", str(rel)).upper()


def check_guard(path: Path, text: str) -> Iterator[Finding]:
    want = expected_guard(path)
    if (match := ANCHORED_GUARD_RE.match(text)) is None:
        if (stray := GUARD_RE.search(text)) is not None:
            line = text.count("\n", 0, stray.start()) + 1
            yield Finding(path, line, "include guard must open the header; only comments before")
        else:
            yield Finding(path, 1, f"missing include guard (expected {want})")
        return
    if (have := match.group(1)) != want:
        line = text.count("\n", 0, match.start(1)) + 1
        yield Finding(path, line, f"include guard {have} should be {want}")
        return
    lines = text.rstrip("\n").splitlines()
    if lines[-1] != f"#endif /* {want} */":
        yield Finding(path, len(lines), f"last line should be '#endif /* {want} */'")


def check_includes(path: Path, text: str) -> Iterator[Finding]:
    for lineno, line in enumerate(text.splitlines(), 1):
        if INCLUDE_DIRECTIVE_RE.match(line) and not INCLUDE_STRICT_RE.match(line):
            yield Finding(path, lineno, 'include must be spelled #include <...> or #include "..."')
            continue
        if (match := INCLUDE_RE.match(line)) is None:
            continue
        include = match.group(1)
        if include.startswith("/") or ".." in Path(include).parts:
            yield Finding(path, lineno, f'include "{include}" must be a plain path under a root')
            continue
        # Quote includes search the including file's directory before the
        # -iquote roots, so a local file wins over a root one of the same name;
        # the canonical spelling must name whichever file the compiler picks.
        local = Path(os.path.normpath(path.parent / include))
        if local.is_file():
            for root in ROOTS:
                if local.is_relative_to(root):
                    canonical = local.relative_to(root)
                    if str(canonical) != include:
                        yield Finding(path, lineno, f'include "{include}" should be "{canonical}"')
                    break
        elif not any((root / include).is_file() for root in ROOTS):
            roots = " or ".join(map(str, ROOTS))
            yield Finding(path, lineno, f'include "{include}" not found under {roots}')


def check_include_comments(path: Path, text: str) -> Iterator[Finding]:
    for lineno, line in enumerate(text.splitlines(), 1):
        if (match := INCLUDE_TRAIL_RE.match(line)) is None:
            continue
        trail = match.group(1)
        if "IWYU pragma:" in trail or "NOLINT" in trail:
            continue
        yield Finding(path, lineno, "include lines take no comments except IWYU pragma/NOLINT")


def check_comment_delimiters(path: Path, text: str) -> Iterator[Finding]:
    for lineno, line in enumerate(text.splitlines(), 1):
        if LONE_DELIMITER_RE.match(line):
            yield Finding(
                path, lineno, "comment delimiter on its own line; put it beside the text"
            )


def check_mkdtemp(path: Path, text: str) -> Iterator[Finding]:
    if not path.is_relative_to("tests") or path == HARNESS:
        return
    for lineno, line in enumerate(text.splitlines(), 1):
        if "mkdtemp" in line:
            yield Finding(path, lineno, "raw mkdtemp in tests; use t_tempdir() from tests/harness.h")


def check_file(path: Path) -> Iterator[Finding]:
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".h":
        yield from check_guard(path, text)
    yield from check_includes(path, text)
    yield from check_include_comments(path, text)
    yield from check_comment_delimiters(path, text)
    yield from check_mkdtemp(path, text)


def source_files() -> Iterator[Path]:
    for root in ROOTS:
        yield from sorted(p for p in root.rglob("*") if p.suffix in {".c", ".h"})


def main() -> int:
    os.chdir(Path(__file__).resolve().parent.parent)
    findings = [finding for path in source_files() for finding in check_file(path)]
    for finding in findings:
        print(finding)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
