"""Shared helpers for pointing a downstream package at a published hax release.

Used by bump_homebrew.py and bump_aur.py. Both take a published version, rewrite a
recipe in a packaging repository, and push it; only the recipe format and the transport
differ.

The checksum is taken from the release asset as served, not from a local build, so the
recipe attests to the artifact users actually download.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys
import urllib.request
from pathlib import Path
from typing import NoReturn

REPO = "OleksandrChekhovskyi/hax"
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")

# GitHub attributes a commit to the Actions bot by this exact address, built from the
# bot account's numeric id: <id>+<login>@users.noreply.github.com. It resolves to nothing
# outside GitHub, but still records an AUR commit as made by automation rather than by a
# person.
_BOT_NAME = "github-actions[bot]"
_BOT_EMAIL = "41898282+github-actions[bot]@users.noreply.github.com"
BOT_IDENTITY = {
    "GIT_AUTHOR_NAME": _BOT_NAME,
    "GIT_AUTHOR_EMAIL": _BOT_EMAIL,
    "GIT_COMMITTER_NAME": _BOT_NAME,
    "GIT_COMMITTER_EMAIL": _BOT_EMAIL,
}


_SECRETS: list[str] = []


def hide_secret(value: str) -> None:
    """Scrub a credential from error output. A failed clone quotes the remote URL, which
    carries the token; CI masks its own secrets, a local run would print it verbatim."""
    if value:
        _SECRETS.append(value)


def die(message: str) -> NoReturn:
    for secret in _SECRETS:
        message = message.replace(secret, "***")
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def parse_version(version: str) -> tuple[int, int, int]:
    """Parse X.Y.Z. Prerelease tags are rejected: a package tracks one version, and
    publishing a prerelease would replace the stable install for every user."""
    match = VERSION_RE.match(version)
    if not match:
        die(f"'{version}' is not a stable X.Y.Z version")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def check_not_downgrade(current: str, new: str) -> None:
    """Refuse to move a recipe backwards, so re-running an old release's failed bump
    after a newer one has shipped cannot roll the package back. Equal versions pass:
    re-running the same bump is how a partial failure is recovered."""
    if parse_version(current) > parse_version(new):
        die(f"package is at {current}, newer than {new}; refusing downgrade")


def source_url(version: str) -> str:
    return f"https://github.com/{REPO}/releases/download/v{version}/hax-{version}.tar.xz"


def fetch_sha256(url: str) -> str:
    try:
        with urllib.request.urlopen(url) as response:
            return hashlib.sha256(response.read()).hexdigest()
    except OSError as err:
        die(f"cannot fetch {url}: {err}")


def extract_once(text: str, pattern: str, what: str) -> str:
    """Return the first group of a regex that must match exactly once."""
    matches = re.findall(pattern, text, flags=re.MULTILINE)
    if len(matches) != 1:
        die(f"expected one {what} in the recipe, found {len(matches)}")
    return matches[0]


def substitute_once(text: str, pattern: str, replacement: str, what: str) -> str:
    """Apply a regex that must match exactly once, replacing it with a literal. A recipe
    whose format has drifted would otherwise be committed unchanged, publishing a version
    that does not match its own metadata. The replacement is inserted verbatim: read as a
    template, a backslash in a URL would expand as a group reference."""
    result, count = re.subn(pattern, lambda _: replacement, text, flags=re.MULTILINE)
    if count != 1:
        die(f"expected one {what} in the recipe, found {count}")
    return result


def git(*args: str, cwd: Path | None = None, env: dict[str, str] | None = None) -> str:
    proc = subprocess.run(
        ("git", *args),
        cwd=cwd,
        capture_output=True,
        text=True,
        env={**os.environ, **env} if env else None,
    )
    if proc.returncode != 0:
        # git reports some failures only on stdout, so stderr alone can be empty.
        detail = "; ".join(s.strip() for s in (proc.stderr, proc.stdout) if s.strip())
        die(f"git {args[0]} failed: {detail or 'no output'}")
    return proc.stdout.strip()


def commit_and_push(repo: Path, message: str, env: dict[str, str] | None = None) -> bool:
    """Commit the recipe and push it. Returns False when the recipe already matches, so
    the caller can report that rather than treat it as a failure: the checks above catch
    every version that is wrong, leaving "unchanged" to mean the bump already landed.

    Identity comes from the environment rather than `git -c` so that a failing command
    reports as `git commit` instead of the whole identity preamble."""
    if not git("status", "--porcelain", "--untracked-files=no", cwd=repo):
        return False
    git("commit", "-am", message, cwd=repo, env={**BOT_IDENTITY, **(env or {})})
    git("push", cwd=repo, env=env)
    return True
