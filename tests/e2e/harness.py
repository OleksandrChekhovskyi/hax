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
    # Never fork real power-management helpers (caffeinate / systemd-inhibit).
    env["HAX_KEEP_AWAKE"] = "0"
    return env


def make_home() -> tuple[Path, Path]:
    """Scratch HOME plus the work directory inside it, shareable across runs of one scenario."""
    home = scratch_dir()
    workdir = home / "work"
    workdir.mkdir()
    return home, workdir


def hax_binary() -> Path:
    # Resolve before any cwd switch so a relative HAX_BIN keeps meaning what
    # the caller wrote.
    return Path(os.environ.get("HAX_BIN", str(REPO_ROOT / "build" / "hax"))).resolve()


def mock_env(home: Path, mock_script: str, extra_env: dict[str, str] | None = None) -> dict[str, str]:
    env = hermetic_env(home)
    env["HAX_PROVIDER"] = "mock"
    env["HAX_MOCK_SCRIPT"] = str(REPO_ROOT / "scripts" / "mock" / mock_script)
    if extra_env:
        env.update(extra_env)
    return env


def run_oneshot(
    prompt: str,
    mock_script: str,
    extra_args: list[str] | None = None,
    extra_env: dict[str, str] | None = None,
) -> Result:
    """Run `hax -p [extra_args] <prompt>` against scripts/mock/<mock_script> in a scratch cwd."""
    home, workdir = make_home()
    # Decode as UTF-8 regardless of the host locale.
    proc = subprocess.run(
        [str(hax_binary()), "-p", *(extra_args or []), prompt],
        cwd=workdir,
        env=mock_env(home, mock_script, extra_env),
        capture_output=True,
        encoding="utf-8",
        timeout=30,
    )
    return Result(proc, workdir)


def spawn_hax(
    args: list[str],
    mock_script: str,
    home: Path,
    workdir: Path,
    extra_env: dict[str, str] | None = None,
) -> subprocess.Popen:
    """Start `hax <args>` for scenarios that drive stdout and signals themselves."""
    return subprocess.Popen(
        [str(hax_binary()), *args],
        cwd=workdir,
        env=mock_env(home, mock_script, extra_env),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
    )


def spawned_result(proc: subprocess.Popen, stdout: str, stderr: str, workdir: Path) -> Result:
    """Package a finished spawn_hax() run so expect() can dump it on failure."""
    completed = subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)
    return Result(completed, workdir)


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
