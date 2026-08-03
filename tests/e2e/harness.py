"""Hermetic driver for the end-to-end scenarios in this directory.

Scenarios are standalone Python scripts registered in `e2e_scenarios` in
tests/meson.build. Each run gets a scratch HOME and working directory
(removed at process exit) and an environment stripped of inherited
HAX_*/XDG_* settings, so scenarios neither depend on nor touch the
developer's real configuration and sessions.

HAX_BIN selects the binary under test; it defaults to build/hax so a
scenario can also be run directly from the repo root.
"""

from __future__ import annotations

import atexit
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class Result:
    """Outcome of one binary run plus the scratch directory it ran in."""

    def __init__(self, proc: subprocess.CompletedProcess, workdir: Path):
        self.returncode = proc.returncode
        self.stdout = proc.stdout
        self.stderr = proc.stderr
        self.workdir = workdir


def scratch_dir() -> Path:
    path = tempfile.mkdtemp(prefix="hax-e2e-")
    atexit.register(shutil.rmtree, path, ignore_errors=True)
    return Path(path)


def hermetic_env(home: Path) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("HAX_") and not key.startswith("XDG_")
    }
    env["HOME"] = str(home)
    return env


def run_oneshot(prompt: str, mock_script: str) -> Result:
    """Run `hax -p <prompt>` against scripts/mock/<mock_script> in a scratch cwd."""
    home = scratch_dir()
    workdir = home / "work"
    workdir.mkdir()
    env = hermetic_env(home)
    env["HAX_PROVIDER"] = "mock"
    env["HAX_MOCK_SCRIPT"] = str(REPO_ROOT / "scripts" / "mock" / mock_script)
    # Resolve before the cwd switch so a relative HAX_BIN keeps meaning what
    # the caller wrote; decode as UTF-8 regardless of the host locale.
    binary = Path(os.environ.get("HAX_BIN", str(REPO_ROOT / "build" / "hax"))).resolve()
    proc = subprocess.run(
        [str(binary), "-p", prompt],
        cwd=workdir,
        env=env,
        capture_output=True,
        encoding="utf-8",
        timeout=30,
    )
    return Result(proc, workdir)


def expect(condition: bool, description: str, result: Result | None = None) -> None:
    """Check a scenario condition; on failure dump the run's output and exit 1."""
    if condition:
        return
    print(f"FAIL: {description}", file=sys.stderr)
    if result is not None:
        print(f"exit status: {result.returncode}", file=sys.stderr)
        for label, text in (("stdout", result.stdout), ("stderr", result.stderr)):
            print(f"--- {label} ---", file=sys.stderr)
            print(text, end="" if text.endswith("\n") else "\n", file=sys.stderr)
    sys.exit(1)


def skip(reason: str) -> None:
    """Exit with meson's skip code, e.g. when a required tool is unavailable."""
    print(f"SKIP: {reason}", file=sys.stderr)
    sys.exit(77)
