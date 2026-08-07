#!/usr/bin/env python3
"""Wait for the GitHub Actions run covering a commit and report its outcome.

Usage: scripts/ci_wait.py [-t seconds] [-b branch] [ref]

A green run prints one line, plus any compiler warnings it built through; a red
one prints the failing jobs and the tail of their logs. Warnings are reported,
never fatal: the exit status carries the whole result, so callers never have to
parse the output:

- 0 green
- 1 red
- 2 usage or environment error
- 3 still running when the timeout expired
- 4 no run found, or the run was superseded by a newer push

Runs are matched by commit within the repository gh resolves from the checkout, so a pull
request opened from a fork is out of scope: its checks run upstream. Interrupting the wait exits
130 and losing the reader mid-report exits 141, as the usual signal conventions.
"""

from __future__ import annotations

import argparse
import enum
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Final, NamedTuple, NoReturn, TextIO

ANSI_GREEN: Final = "32"
ANSI_RED: Final = "31"
ANSI_YELLOW: Final = "33"

POLL_SECONDS: Final = 5.0
# GitHub registers a run shortly after the push, so a run counts as missing only once this grace
# period has passed.
DISCOVERY_SECONDS: Final = 90.0
DEFAULT_TIMEOUT_SECONDS: Final = 1800
LOG_TAIL_LINES: Final = 40
RUN_LIST_LIMIT: Final = 30
WARNING_REPORT_LIMIT: Final = 12
# A wait may span half an hour; transient gh or network trouble must not end it.
MAX_QUERY_FAILURES: Final = 3

# gh's log output prefixes every line with the job name, the step name, and a timestamp, and
# renders escape sequences in caret notation, as two literal characters rather than an ESC byte.
LOG_LINE_RE: Final = re.compile(
    r"^(?P<job>[^\t]*)\t[^\t]*\t\ufeff?(?:\d{4}-\d\d-\d\dT[\d:.]+Z )?(?P<text>.*)$"
)
CARET_ESCAPE_RE: Final = re.compile(r"\^\[\[[0-9;]*[mK]")

# A compiler diagnostic, recognised by its source position: the word alone would drag in the
# package-manager and checkout chatter that fills a green run's logs. The cost is that
# positionless linker and driver warnings go unreported until they turn into failures.
WARNING_RE: Final = re.compile(r"^(?P<where>\S+:\d+(?::\d+)?): warning: (?P<text>.+)$")


class Exit(enum.IntEnum):
    GREEN = 0
    RED = 1
    USAGE = 2
    TIMEOUT = 3
    NO_RUN = 4


PUSH_REMOTE: Final = "origin"


class Repo(NamedTuple):
    """The repository every query is aimed at."""

    host: str
    slug: str

    @property
    def spec(self) -> str:
        """The [HOST/]OWNER/REPO that gh's --repo and positional repository arguments take."""
        return self.slug if self.host == "github.com" else f"{self.host}/{self.slug}"

    @property
    def api_host(self) -> tuple[str, ...]:
        return () if self.host == "github.com" else ("--hostname", self.host)


def origin_repo() -> Repo | None:
    """The repository behind the push remote, or None where its URL does not name one.

    gh chooses a repository from its own configuration and remote preferences, which need not
    be the remote that gets pushed. Binding every query to this one keeps the checks and the
    push talking about the same repository.
    """
    # The push URL is what land actually writes to, and it need not be the fetch URL. Several
    # of them means no single repository is the one being landed on.
    urls = (git("remote", "get-url", "--push", "--all", PUSH_REMOTE) or "").splitlines()
    if len(urls) != 1:
        return None
    url = urls[0]
    url = url.removesuffix(".git")
    if "://" in url:
        host, _, path = url.split("://", 1)[1].split("@")[-1].partition("/")
    else:
        before, sep, path = url.partition(":")  # scp-like [user@]host:owner/repo
        if not sep:
            return None
        host = before.split("@")[-1]
    parts = [part for part in path.split("/") if part]
    if len(parts) < 2:
        return None
    return Repo(host or "github.com", "/".join(parts[-2:]))


class Snapshot(NamedTuple):
    status: str
    conclusion: str
    jobs_done: int
    jobs_total: int
    url: str


def paint(text: str, color: str, stream: TextIO) -> str:
    """Colour the text for a terminal that wants it.

    Decoration only: the symbol beside each verdict carries the same reading where colour
    cannot, down a pipe or under NO_COLOR.
    """
    if "NO_COLOR" in os.environ or not stream.isatty():
        return text
    return f"\033[{color}m{text}\033[0m"


def die(message: str, code: Exit) -> NoReturn:
    print(f"{paint('error:', ANSI_RED, sys.stderr)} {message}", file=sys.stderr)
    sys.exit(code)


def format_elapsed(seconds: float) -> str:
    whole = int(seconds)
    if whole >= 3600:
        return f"{whole // 3600}h{whole % 3600 // 60:02d}m"
    return f"{whole // 60}m{whole % 60:02d}s"


def git(*args: str) -> str | None:
    """Run a git command, returning its trimmed output or None if it failed."""
    proc = subprocess.run(("git", *args), capture_output=True, text=True)
    return proc.stdout.strip() if proc.returncode == 0 else None


class Progress:
    """Status output for a wait that may last half an hour.

    A terminal gets a status line redrawn in place; anything else gets one line per state change,
    keeping a long wait down to a few lines of scrollback or agent context. The elapsed figure the
    status line carries is the wait's own clock, so the deadlines are measured against it too.

    Redrawn status is left unterminated, so the line is owned for a scope: however the wait ends,
    Ctrl-C included, it is erased on exit before a report or a shell prompt can land mid-line.
    """

    def __init__(self, label: str) -> None:
        self.label = label
        self.interactive = sys.stdout.isatty()
        self.last_status: str | None = None
        self.line_pending = False
        self.started = time.monotonic()

    def __enter__(self) -> Progress:
        return self

    def __exit__(self, *_exc_info: object) -> None:
        self.clear()

    @property
    def elapsed(self) -> float:
        return time.monotonic() - self.started

    def show(self, status: str, detail: str = "") -> None:
        if self.interactive:
            line = f"CI {self.label} · {status}"
            if detail:
                line += f" · {detail}"
            print(f"\r\033[K{line} · {format_elapsed(self.elapsed)}", end="", flush=True)
            self.line_pending = True
        elif status != self.last_status:
            print(f"CI {self.label}: {status}", flush=True)
        self.last_status = status

    def clear(self) -> None:
        if self.line_pending:
            # Erasing the line takes the terminal's own "^C" echo with it, leaving the cursor
            # where a prompt can start; a newline would keep both.
            print("\r\033[K", end="", flush=True)
            self.line_pending = False


class Gh:
    """gh invocations that ride out transient failures but give up on persistent ones."""

    def __init__(self, progress: Progress, repo: Repo) -> None:
        self.progress = progress
        self.repo = repo
        self.consecutive_failures = 0

    def run(self, *args: str) -> str | None:
        proc = subprocess.run(("gh", *args), capture_output=True, text=True)
        if proc.returncode == 0:
            self.consecutive_failures = 0
            return proc.stdout
        self.consecutive_failures += 1
        if self.consecutive_failures >= MAX_QUERY_FAILURES:
            self.progress.clear()
            print(proc.stderr.strip(), file=sys.stderr)
            die(f"gh query failed {self.consecutive_failures} times in a row", Exit.USAGE)
        return None

    def json(self, *args: str) -> Any | None:
        output = self.run(*args)
        if output is None:
            return None
        try:
            return json.loads(output)
        except json.JSONDecodeError:
            return None


def find_run_id(gh: Gh, sha: str, branch: str | None) -> str | None:
    """Newest run for the commit, or None until GitHub has registered the push."""
    # --commit matches the full forty-character SHA only, and silently matches nothing when given
    # an abbreviation. Filtering server-side also keeps an older run in reach, where scanning a
    # fixed window of recent runs would lose it behind newer ones.
    args = ["run", "list", "-R", gh.repo.spec, "--commit", sha,
            "--limit", str(RUN_LIST_LIMIT), "--json", "databaseId"]
    if branch is not None:
        args += ["--branch", branch]
    runs = gh.json(*args)
    if not isinstance(runs, list) or not runs:
        return None
    # gh lists newest first. One commit can carry several runs: one per ref it was pushed to, and
    # a second on the same branch once a same-repo pull request is open. The branch narrows the
    # field, and the newest of what remains wins.
    return str(runs[0]["databaseId"])


# gh reports an unknown commit as HTTP 422 ("No commit found for SHA") and a missing repository
# as 404. A connection failure carries no status at all and settles nothing.
FORGE_DENIES_COMMIT_RE: Final = re.compile(r"HTTP (?:404|422)")


def commit_pushed(sha: str, repo: Repo) -> bool | None:
    """Whether the forge holds the commit, or None when it could not say.

    Asked instead of consulting remote-tracking refs, which go stale between fetches.
    """
    proc = subprocess.run(
        ("gh", "api", f"repos/{repo.slug}/commits/{sha}", *repo.api_host, "--silent"),
        capture_output=True,
        text=True,
    )
    if proc.returncode == 0:
        return True
    return False if FORGE_DENIES_COMMIT_RE.search(proc.stderr) else None


def repo_is_fork(gh: Gh) -> bool:
    # gh repo view takes its repository positionally; it has no --repo flag.
    data = gh.json("repo", "view", gh.repo.spec, "--json", "isFork")
    return isinstance(data, dict) and bool(data.get("isFork"))


def view_run(gh: Gh, run_id: str) -> Snapshot | None:
    data = gh.json("run", "view", run_id, "-R", gh.repo.spec,
                   "--json", "status,conclusion,url,jobs")
    if not isinstance(data, dict):
        return None
    jobs = data.get("jobs") or []
    return Snapshot(
        status=data.get("status") or "",
        conclusion=data.get("conclusion") or "",
        jobs_done=sum(1 for job in jobs if job.get("status") == "completed"),
        jobs_total=len(jobs),
        url=data.get("url") or "",
    )


class LogLine(NamedTuple):
    job: str
    text: str


def parse_log(log: str) -> list[LogLine]:
    """Split gh log output into the job each line came from and the line the job printed."""
    lines: list[LogLine] = []
    for raw in log.splitlines():
        match = LOG_LINE_RE.match(raw)
        job, text = (match.group("job"), match.group("text")) if match else ("", raw)
        lines.append(LogLine(job, CARET_ESCAPE_RE.sub("", text)))
    return lines


def report_failure(gh: Gh, run_id: str) -> None:
    data = gh.json("run", "view", run_id, "-R", gh.repo.spec, "--json", "jobs")
    if isinstance(data, dict):
        failed = [
            job["name"]
            for job in data.get("jobs") or []
            if job.get("conclusion") not in (None, "success", "skipped")
        ]
        if failed:
            print(f"failed jobs: {', '.join(failed)}", file=sys.stderr)

    log = gh.run("run", "view", run_id, "-R", gh.repo.spec, "--log-failed")
    if not log:
        return
    # The failing jobs are named above; a per-line prefix would only spend width.
    tail = parse_log(log)[-LOG_TAIL_LINES:]
    print(f"--- last {LOG_TAIL_LINES} log lines ---", file=sys.stderr)
    for line in tail:
        print(line.text, file=sys.stderr)


def collect_warnings(log: str) -> dict[str, list[str]]:
    """Compiler warnings in the log, each mapped to the jobs that emitted it.

    A warning in shared code repeats across the matrix, so the jobs are gathered per warning
    rather than the warnings reported once per job.
    """
    warnings: dict[str, list[str]] = {}
    for line in parse_log(log):
        match = WARNING_RE.match(line.text)
        if not match:
            continue
        jobs = warnings.setdefault(f"{match.group('where')}: {match.group('text')}", [])
        if line.job and line.job not in jobs:
            jobs.append(line.job)
    return warnings


def report_warnings(gh: Gh, run_id: str) -> None:
    """Note the warnings a green run compiled through, which nothing else would report."""
    log = gh.run("run", "view", run_id, "-R", gh.repo.spec, "--log")
    if not log:
        return
    warnings = collect_warnings(log)
    if not warnings:
        return
    count = len(warnings)
    header = paint(f"! {count} warning{'' if count == 1 else 's'}", ANSI_YELLOW, sys.stderr)
    print(header, file=sys.stderr)
    for warning, jobs in list(warnings.items())[:WARNING_REPORT_LIMIT]:
        print(f"  {warning}", file=sys.stderr)
        if jobs:
            print(f"    in {', '.join(sorted(jobs))}", file=sys.stderr)
    if count > WARNING_REPORT_LIMIT:
        print(f"  ... and {count - WARNING_REPORT_LIMIT} more", file=sys.stderr)


def whole_seconds(value: str) -> int:
    if not value.isdigit():
        raise argparse.ArgumentTypeError(f"expected a whole number of seconds, got {value!r}")
    return int(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Wait for the GitHub Actions run covering a commit.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "-t",
        "--timeout",
        type=whole_seconds,
        default=DEFAULT_TIMEOUT_SECONDS,
        metavar="SECONDS",
        help="give up waiting after this long (default: %(default)s)",
    )
    parser.add_argument(
        "-b",
        "--branch",
        metavar="BRANCH",
        help="branch whose run to watch (default: the current branch)",
    )
    parser.add_argument("ref", nargs="?", default="HEAD", help="commit to wait for")
    return parser.parse_args()


def discover_run(
    gh: Gh, progress: Progress, sha: str, short_sha: str, branch: str | None, deadline: float
) -> str | Exit:
    """Poll until the forge registers a run for the commit.

    Returns the run id, or the status the whole wait ends on when no run is coming.
    """
    asked_whether_pushed = False
    while True:
        run_id = find_run_id(gh, sha, branch)
        if run_id is not None:
            return run_id

        # A commit the forge has never seen can never gather a run, so spending the discovery
        # window on one only delays the answer. Ask once, on the first miss.
        if not asked_whether_pushed:
            asked_whether_pushed = True
            if commit_pushed(sha, gh.repo) is False:
                progress.clear()
                print(f"error: {short_sha} is not on the remote; push it first", file=sys.stderr)
                return Exit.NO_RUN

        if progress.elapsed >= deadline:
            progress.clear()
            branch_note = f" on {branch}" if branch else ""
            prefix = paint("error:", ANSI_RED, sys.stderr)
            print(f"{prefix} no CI run for {short_sha}{branch_note}", file=sys.stderr)
            if repo_is_fork(gh):
                print(
                    "a pull request's checks run in the upstream repository, not this fork; "
                    "try: gh pr checks",
                    file=sys.stderr,
                )
            return Exit.NO_RUN

        progress.show("no run yet")
        time.sleep(POLL_SECONDS)


def await_completion(
    gh: Gh, progress: Progress, run_id: str, short_sha: str, timeout: float
) -> Snapshot | Exit:
    """Poll the run until it completes.

    Returns the completed snapshot, or the status the whole wait ends on when the timeout
    overtakes the run.
    """
    while True:
        snapshot = view_run(gh, run_id)
        if snapshot is None:
            time.sleep(POLL_SECONDS)
            continue
        if snapshot.status == "completed":
            return snapshot

        status = "running" if snapshot.status == "in_progress" else snapshot.status
        progress.show(status, f"{snapshot.jobs_done}/{snapshot.jobs_total} jobs")
        if progress.elapsed >= timeout:
            progress.clear()
            verdict = paint(f"! CI {short_sha}: still running", ANSI_YELLOW, sys.stderr)
            waited = format_elapsed(progress.elapsed)
            print(f"{verdict} after {waited} · {snapshot.url}", file=sys.stderr)
            return Exit.TIMEOUT
        time.sleep(POLL_SECONDS)


def wait_for_run(sha: str, branch: str | None, timeout: float, repo: Repo) -> Exit:
    """Wait for the run covering the commit and report its outcome to the terminal.

    Takes a full SHA and reports through the return value, so a caller in this process reads
    the same verdict the command line does. The working directory must already be one gh can
    resolve the repository from.
    """
    short_sha = git("rev-parse", "--short", sha) or sha[:7]

    with Progress(short_sha) as progress:
        gh = Gh(progress, repo)
        run_id = discover_run(
            gh, progress, sha, short_sha, branch, min(DISCOVERY_SECONDS, timeout)
        )
        if isinstance(run_id, Exit):
            return run_id
        snapshot = await_completion(gh, progress, run_id, short_sha, timeout)
        if isinstance(snapshot, Exit):
            return snapshot
        elapsed = progress.elapsed

    if snapshot.conclusion == "success":
        verdict = paint("✓ CI green", ANSI_GREEN, sys.stdout)
        print(
            f"{verdict}: {short_sha} · {snapshot.jobs_total} jobs · "
            f"waited {format_elapsed(elapsed)}"
        )
        report_warnings(gh, run_id)
        return Exit.GREEN
    if snapshot.conclusion == "cancelled":
        verdict = paint(f"! CI {short_sha}: cancelled", ANSI_YELLOW, sys.stderr)
        print(
            f"{verdict}, likely superseded by a newer push · {snapshot.url}",
            file=sys.stderr,
        )
        return Exit.NO_RUN

    verdict = paint(f"✗ CI {short_sha}: {snapshot.conclusion}", ANSI_RED, sys.stderr)
    print(f"{verdict} · {snapshot.url}", file=sys.stderr)
    report_failure(gh, run_id)
    return Exit.RED


def main() -> int:
    args = parse_args()
    os.chdir(Path(__file__).resolve().parent.parent)

    if shutil.which("gh") is None:
        die("gh not found; install the GitHub CLI", Exit.USAGE)

    sha = git("rev-parse", "--verify", "--quiet", f"{args.ref}^{{commit}}")
    if not sha:
        die(f"not a commit: {args.ref}", Exit.USAGE)

    branch = args.branch
    if branch is None:
        current = git("rev-parse", "--abbrev-ref", "HEAD")
        branch = None if current in (None, "HEAD") else current

    repo = origin_repo()
    if repo is None:
        die(f"cannot tell which repository {PUSH_REMOTE} points at", Exit.USAGE)

    return wait_for_run(sha, branch, args.timeout, repo)


if __name__ == "__main__":
    # Interleaving with stderr is the whole report's legibility, and a block-buffered
    # stdout would hold a verdict back past the error that followed it.
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(line_buffering=True)
    try:
        status = main()
    except KeyboardInterrupt:
        status = 130
    except BrokenPipeError:
        status = 141
    # A reader that closed early (`| head`) would otherwise strand a broken stream for the
    # interpreter to flush at exit, which reports 120 with a traceback. Retire both streams to
    # the void so the status stays the deliberate one.
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    except BrokenPipeError:
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, sys.stdout.fileno())
        os.dup2(devnull, sys.stderr.fileno())
    sys.exit(status)
