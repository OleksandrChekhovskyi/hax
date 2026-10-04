"""Hermetic driver for the end-to-end scenarios in this directory.

Scenario files are standalone Python scripts, one per user-facing area, registered in
`e2e_scenarios` in tests/meson.build. A file defines one test_* function per scenario and ends with
`harness.main(globals())`, which runs them in order, reports each failure under its function's
name, and exits 1 if any failed. Each run gets a scratch HOME and working directory (removed at
process exit) and an environment stripped of inherited HAX_*/XDG_* settings, so scenarios neither
depend on nor touch the developer's real configuration and sessions.

HAX_BIN selects the binary under test; it defaults to build/hax so a file can also be run directly
from the repo root, optionally naming the scenarios to run: `test_oneshot.py test_json_stream`.

REPL scenarios drive hax through a Terminal, which needs tmux (scripts/install_deps.sh tests).
HAX_E2E_KEEP=1 keeps a failed scenario's tmux servers and scratch directories and prints where to
find them.
"""

from __future__ import annotations

import atexit
import difflib
import errno
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import traceback
from collections.abc import Callable
from pathlib import Path
from typing import NamedTuple

REPO_ROOT = Path(__file__).resolve().parents[2]

# Meson stops a hung scenario with SIGTERM; exiting normally runs the atexit cleanup that stops its
# tmux servers, which would otherwise outlive it.
signal.signal(signal.SIGTERM, lambda signum, _: sys.exit(128 + signum))


class Result:
    """Outcome of one binary run plus the scratch directory it ran in."""

    def __init__(self, proc: subprocess.CompletedProcess, workdir: Path):
        self.returncode = proc.returncode
        self.stdout = proc.stdout
        self.stderr = proc.stderr
        self.workdir = workdir


# The running scenario's directories, which main() removes or keeps when it ends; atexit covers
# runs that end some other way.
_scratch_dirs: list[str] = []
_kept_dirs: set[str] = set()


def _remove_scratch(path: str) -> None:
    if path not in _kept_dirs:
        shutil.rmtree(path, ignore_errors=True)


def scratch_dir() -> Path:
    path = tempfile.mkdtemp(prefix="hax-e2e-")
    _scratch_dirs.append(path)
    atexit.register(_remove_scratch, path)
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
    """Environment for one run of the mock provider. `mock_script` is the script's text, indented
    like an expected screen block; docs/debugging.md describes its directives."""
    # One file per run: the provider rereads its script on every request, and runs may share home.
    fd, script_path = tempfile.mkstemp(prefix="mock-", suffix=".txt", dir=home)
    with os.fdopen(fd, "w") as script:
        script.write(textwrap.dedent(mock_script))
    env = hermetic_env(home)
    env["HAX_PROVIDER"] = "mock"
    env["HAX_MOCK_SCRIPT"] = script_path
    if extra_env:
        env.update(extra_env)
    return env


def run_oneshot(
    prompt: str,
    mock_script: str,
    extra_args: list[str] | None = None,
    extra_env: dict[str, str] | None = None,
) -> Result:
    """Run `hax -p [extra_args] <prompt>` against `mock_script` in a scratch cwd."""
    home, workdir = make_home()
    # Decode as UTF-8 regardless of the host locale.
    proc = subprocess.run(
        [str(hax_binary()), "-p", *(extra_args or []), prompt],
        cwd=workdir,
        env=mock_env(home, mock_script, extra_env),
        stdin=subprocess.DEVNULL,
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
    # hax reads a prompt from a stdin that is not a terminal, so never hand it the runner's.
    return subprocess.Popen(
        [str(hax_binary()), *args],
        cwd=workdir,
        env=mock_env(home, mock_script, extra_env),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
    )


def spawned_result(proc: subprocess.Popen, stdout: str, stderr: str, workdir: Path) -> Result:
    """Package a finished spawn_hax() run so expect() can dump it on failure."""
    completed = subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)
    return Result(completed, workdir)


# Meson captures scenario output, so color reaches only a scenario run directly in a terminal.
_COLOR = sys.stderr.isatty() and not os.environ.get("NO_COLOR")
_DIM, _RED, _GREEN, _BOLD_RED = "2", "31", "32", "1;31"


def _paint(sgr: str, text: str) -> str:
    return f"\x1b[{sgr}m{text}\x1b[0m" if _COLOR else text


class ScenarioFailure(Exception):
    """A failed check; `details` is the evidence main() prints below the description."""

    def __init__(self, description: str, details: str = ""):
        super().__init__(description)
        self.details = details


def expect(condition: bool, description: str, result: Result | None = None) -> None:
    """Check a scenario condition; on failure end the scenario, dumping the run's output."""
    if condition:
        return
    details = []
    if result is not None:
        details.append(_paint(_DIM, f"exit status: {result.returncode}"))
        for label, text in (("stdout", result.stdout), ("stderr", result.stderr)):
            details += [_paint(_DIM, f"--- {label} ---"), text.rstrip("\n")]
    raise ScenarioFailure(description, "\n".join(details))


def main(namespace: dict) -> None:
    """Run the test_* functions in `namespace` in definition order, or only those named on the
    command line, and exit with the outcome. A failure ends only its own scenario, and each
    scenario's tmux servers and scratch directories go when it ends."""
    tests = {
        name: test
        for name, test in namespace.items()
        if name.startswith("test_") and callable(test)
    }
    unknown = [name for name in sys.argv[1:] if name not in tests]
    if unknown:
        print(f"unknown scenario: {', '.join(unknown)}; have {', '.join(tests)}", file=sys.stderr)
        sys.exit(2)
    failures = 0
    for name in sys.argv[1:] or tests:
        test = tests[name]
        failed = True
        try:
            test()
            failed = False
        except ScenarioFailure as failure:
            failures += 1
            print(_paint(_BOLD_RED, f"FAIL {name}: {failure}"), file=sys.stderr)
            if failure.details:
                print(failure.details, file=sys.stderr)
        except Exception:
            failures += 1
            print(_paint(_BOLD_RED, f"FAIL {name}: unexpected error"), file=sys.stderr)
            traceback.print_exc()
        finally:
            _end_scenario(keep=failed and os.environ.get("HAX_E2E_KEEP") == "1")
    if failures:
        print(f"{failures} failures", file=sys.stderr)
    sys.exit(1 if failures else 0)


def _end_scenario(keep: bool) -> None:
    """Stop the scenario's tmux servers and remove its scratch directories, or keep them all for
    inspection and say where they are."""
    for term in _terminals:
        if keep:
            term._keep = True
            print(f"kept: {shlex.join([*term._tmux, 'attach', '-t', 'hax'])}", file=sys.stderr)
        else:
            term.close()
    for path in _scratch_dirs:
        if keep:
            _kept_dirs.add(path)
            print(f"kept: {path}", file=sys.stderr)
        else:
            shutil.rmtree(path, ignore_errors=True)
    _terminals.clear()
    _scratch_dirs.clear()


def skip(reason: str) -> None:
    """Exit with meson's skip code, e.g. when a required tool is unavailable."""
    print(f"SKIP: {reason}", file=sys.stderr)
    sys.exit(77)


# escape-time 0 delivers a lone Esc at once; remain-on-exit keeps the final screen and exit status
# readable; /bin/sh keeps the launch command's quoting independent of the login shell.
_TMUX_CONF = """\
set -sg escape-time 0
set -g status off
set -g remain-on-exit on
set -g default-shell /bin/sh
"""

_WILDCARD = "{*}"

# Long enough for a loaded machine to read keys just sent, before an idle prompt counts as final.
_SETTLE_S = 1.0

_terminals: list[Terminal] = []


class _Pane(NamedTuple):
    dead: bool
    status: str  # exit status once dead
    cursor_shown: bool
    cursor_row: int


def _screen_lines(text: str) -> list[str]:
    """Rows without the blank ones above and below the content."""
    lines = [line.rstrip() for line in text.split("\n")]
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return lines


def _line_matches(expected: str, actual: str) -> bool:
    if _WILDCARD not in expected:
        return expected == actual
    pattern = ".*".join(re.escape(part) for part in expected.split(_WILDCARD))
    return re.fullmatch(pattern, actual) is not None


def _screen_diff(expected: list[str], actual: list[str]) -> list[str]:
    """Unified diff that shows wildcard-matched rows as the wildcard row, so they are not reported
    as differences."""
    shown = []
    for row in actual:
        if row in expected:
            shown.append(row)
            continue
        shown.append(next((line for line in expected if _line_matches(line, row)), row))
    return list(difflib.unified_diff(expected, shown, "expected", "actual", lineterm=""))


def _paint_diff_line(line: str) -> str:
    if line.startswith(("---", "+++", "@@")):
        return _paint(_DIM, line)
    if line.startswith("-"):
        return _paint(_RED, line)
    if line.startswith("+"):
        return _paint(_GREEN, line)
    return line


class Terminal:
    """hax in a private tmux server: keys in, visible screen text out.

    The server's socket lives in a scratch directory and the server is killed at exit, so
    scenarios never touch the developer's tmux sessions or each other. Terminals may share a
    home/workdir pair, e.g. to resume a session an earlier run recorded.
    """

    def __init__(
        self,
        mock_script: str,
        args: list[str] | None = None,
        home: Path | None = None,
        workdir: Path | None = None,
        extra_env: dict[str, str] | None = None,
        size: tuple[int, int] = (80, 24),
    ):
        tmux = shutil.which("tmux")
        if tmux is None:
            print(
                "FAIL: tmux not found; scripts/install_deps.sh tests installs it", file=sys.stderr
            )
            sys.exit(1)
        if home is None or workdir is None:
            home, workdir = make_home()
        self.workdir = workdir
        server_dir = scratch_dir()
        conf = server_dir / "tmux.conf"
        conf.write_text(_TMUX_CONF)
        # -u: treat the panes as UTF-8 whatever the host locale says.
        self._tmux = [tmux, "-u", "-S", str(server_dir / "tmux.sock"), "-f", str(conf)]
        self._keep = False
        _terminals.append(self)
        atexit.register(self.close)

        env = mock_env(home, mock_script, extra_env)
        # Not nested in the developer's own tmux, even when the tests run inside it.
        env.pop("TMUX", None)
        env.pop("TMUX_PANE", None)
        # A daemonized server starts panes up to 300ms late on macOS, while one kept in the
        # foreground (-D) as our child starts them at once. Panes inherit the server's
        # environment, so it starts with the scenario's.
        self._server = subprocess.Popen(
            [*self._tmux, "-D"],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        columns, rows = size
        command = shlex.join([str(hax_binary()), *(args or [])])
        # -N: until the server listens, fail rather than start a daemonized one in its place.
        new_session = [*self._tmux, "-N", "new-session", "-d", "-s", "hax"]
        new_session += ["-x", str(columns), "-y", str(rows), "-c", str(workdir), command]
        deadline = time.monotonic() + 10
        while True:
            proc = subprocess.run(new_session, env=env, capture_output=True, encoding="utf-8")
            if proc.returncode == 0:
                break
            if self._server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f"tmux new-session failed: {proc.stderr.strip()}")
            time.sleep(0.005)

    def _run(self, *args: str, env: dict[str, str] | None = None) -> str:
        proc = subprocess.run(
            [*self._tmux, *args], env=env, capture_output=True, encoding="utf-8", check=True
        )
        return proc.stdout

    def close(self) -> None:
        """Stop the tmux server, unless HAX_E2E_KEEP kept it to inspect a failure."""
        if not self._keep:
            subprocess.run([*self._tmux, "kill-server"], capture_output=True)
            try:
                self._server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._server.kill()
                self._server.wait()

    def type(self, text: str) -> None:
        """Send `text` as literal keystrokes."""
        self._run("send-keys", "-t", "hax", "-l", "--", text)

    def send(self, *keys: str) -> None:
        """Send named keys in tmux's spelling: Enter, Escape, C-c, BSpace, ..."""
        self._run("send-keys", "-t", "hax", "--", *keys)

    def screen(self, styles: bool = False) -> str:
        """The visible rows; `styles` keeps the SGR sequences that color and dim them."""
        return self._run("capture-pane", "-p", "-t", "hax", *(["-e"] if styles else []))

    def wait_for(self, text: str, timeout: float = 10) -> None:
        """Wait until `text` appears anywhere on screen."""
        _, stopped = self._poll(lambda screen, _: text in screen, timeout)
        if stopped:
            self._fail(f"{text!r} never appeared on screen: {stopped}")

    def expect_screen(self, expected: str, timeout: float = 10) -> None:
        """Wait until the visible rows equal `expected`, an indented block written as the terminal
        shows it. Blank rows around the content are ignored; {*} matches any text within a row."""
        want = _screen_lines(textwrap.dedent(expected))

        def matches(screen: str, _: _Pane) -> bool:
            have = _screen_lines(screen)
            return len(have) == len(want) and all(map(_line_matches, want, have))

        screen, stopped = self._poll(matches, timeout)
        if stopped:
            self._fail(
                f"screen does not match: {stopped}", _screen_diff(want, _screen_lines(screen))
            )

    def write_fifo(self, path: Path, text: str, timeout: float = 10) -> None:
        """Write `text` to the FIFO at `path` once something in hax opens it for reading. A blocking
        open would wait forever if the reader never comes, so retry a non-blocking one instead."""

        def written(_screen: str, _pane: _Pane) -> bool:
            try:
                fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
            except OSError as error:
                if error.errno == errno.ENXIO:  # no reader yet
                    return False
                raise
            try:
                os.write(fd, text.encode())
            finally:
                os.close(fd)
            return True

        _, stopped = self._poll(written, timeout)
        if stopped:
            self._fail(f"nothing opened {path.name} for reading: {stopped}")

    def wait_exit(self, timeout: float = 10) -> int:
        """Wait for hax to exit and return its exit status."""
        _, stopped = self._poll(lambda _, pane: pane.dead, timeout)
        if stopped:
            self._fail(f"hax did not exit: {stopped}")
        return int(self._pane().status)

    def _pane(self) -> _Pane:
        query = "#{pane_dead},#{pane_dead_status},#{cursor_flag},#{cursor_y}"
        dead, status, cursor, row = (
            self._run("display-message", "-p", "-t", "hax", query).strip().split(",")
        )
        return _Pane(dead == "1", status, cursor == "1", int(row))

    def _poll(self, done: Callable[[str, _Pane], bool], timeout: float) -> tuple[str, str | None]:
        """Poll until `done(screen, pane)`, returning the last screen and None, or that screen and
        why polling stopped short. It stops once nothing can change the screen any more: hax exited,
        or it has waited for input with the screen unchanged for _SETTLE_S. hax shows the cursor
        only while reading input, and anything else it does moves the screen, if only a spinner."""
        deadline = time.monotonic() + timeout
        previous, unchanged_since = None, time.monotonic()
        while True:
            # Pane before screen: once the pane is dead, the screen captured after it is final, so
            # `done` has judged everything hax drew before an exit ends the poll.
            pane = self._pane()
            screen = self.screen()
            if done(screen, pane):
                return screen, None
            now = time.monotonic()
            if screen != previous:
                previous, unchanged_since = screen, now
            if pane.dead:
                return screen, f"hax exited with status {pane.status}"
            rows = screen.split("\n")
            # Before hax draws anything, tmux shows a cursor on a blank row.
            waiting = pane.cursor_shown and rows[pane.cursor_row].strip() != ""
            if waiting and now - unchanged_since >= _SETTLE_S:
                return screen, "hax is waiting for input"
            if now > deadline:
                return screen, f"timed out after {timeout:g}s"
            time.sleep(0.05)

    def _fail(self, description: str, diff: list[str] | None = None) -> None:
        details = [_paint_diff_line(line) for line in diff or []]
        details.append(_paint(_DIM, "--- screen ---"))
        for number, row in enumerate(self.screen().rstrip("\n").split("\n"), 1):
            details.append(_paint(_DIM, f"{number:3} │") + row)
        raise ScenarioFailure(description, "\n".join(details))
