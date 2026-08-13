#!/usr/bin/env python3
"""Fast-forward master onto the current branch once its CI run is green.

Usage: scripts/maint/land.py [-t seconds] [-y]

The branch is never switched, and the remote moves before the local ref: a rejected push
leaves the local one untouched, whereas the reverse order would need a manual rollback.

Landing is confirmed before the CI wait begins, not after: the wait is meant to be left
unattended, and a prompt at the far end of it would stall the landing it gated. Confirmation
is an answer at the prompt or -y; without a terminal to ask, -y is the only way, and its
absence turns the run into a dry run rather than an unattended push to a public branch.

Exit status: 0 landed, 2 usage or environment error, 5 the repository is not in a landable
state, 6 the landing was not confirmed. A CI verdict short of green passes through from
ci_wait unchanged, so 1, 3, and 4 keep the meanings given there.
"""

from __future__ import annotations

import argparse
import io
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

import ci_wait
from ci_wait import ANSI_GREEN, ANSI_RED, Repo, paint

REMOTE: str = ci_wait.PUSH_REMOTE
TARGET: str = "master"
CONFIRM_LOG_LIMIT: int = 10
DEFAULT_TIMEOUT_SECONDS: int = 1800

NOT_LANDABLE: int = 5
NOT_CONFIRMED: int = 6


def refuse(message: str) -> NoReturn:
    print(f"{paint('error:', ANSI_RED, sys.stderr)} {message}", file=sys.stderr)
    sys.exit(NOT_LANDABLE)


def git(*args: str) -> str:
    """Run a git command that is expected to succeed, returning its trimmed output."""
    proc = subprocess.run(("git", *args), capture_output=True, text=True)
    if proc.returncode != 0:
        refuse(f"git {' '.join(args)} failed: {proc.stderr.strip()}")
    return proc.stdout.strip()


def git_succeeds(*args: str) -> bool:
    """Whether a git command exits zero, for the ones asked as questions."""
    return subprocess.run(("git", *args), capture_output=True, text=True).returncode == 0


def rev_parse(ref: str) -> str | None:
    """Resolve a ref, or None where it does not exist."""
    proc = subprocess.run(
        ("git", "rev-parse", "--verify", "--quiet", ref), capture_output=True, text=True
    )
    return proc.stdout.strip() or None


def is_ancestor(earlier: str, later: str) -> bool:
    return git_succeeds("merge-base", "--is-ancestor", earlier, later)


def gh_json(args: list[str], jq: str, env: dict[str, str] | None = None) -> str | None:
    """Run a gh query, returning its trimmed output or None if gh failed."""
    proc = subprocess.run(
        ("gh", *args, "--jq", jq),
        capture_output=True,
        text=True,
        env={**os.environ, **env} if env else None,
    )
    return proc.stdout.strip() if proc.returncode == 0 else None


def refuse_if_target_checked_out() -> None:
    """The update-ref at the end moves the target even where a worktree has it checked out,
    which leaves that worktree's files behind the branch they are on and reports every one of
    them as a modification."""
    path = ""
    for line in git("worktree", "list", "--porcelain").splitlines():
        if line.startswith("worktree "):
            path = line[len("worktree ") :]
        elif line == f"branch refs/heads/{TARGET}":
            refuse(f"{TARGET} is checked out in {path}; move that worktree off {TARGET} first")


def refuse_if_pull_request_open(branch: str, repo: Repo) -> None:
    """A branch with an open pull request lands through it instead: gh records the merge
    against the PR, honors its merge strategy and required checks, and waits server-side
    rather than holding this machine open for the length of a CI run.

    gh pr list --head matches on branch name alone and takes no owner, so a fork's branch of
    the same name answers to it too, and no page size makes filtering afterwards sound. The
    REST head filter takes owner:branch and narrows before the page is cut; the repository is
    then compared outright, since one account can hold both this repository and a fork of it.

    A branch name may hold & or #, which would cut the query short were it spliced into the
    path, so the parameters are passed as parameters and gh encodes them.
    """
    owner = repo.slug.split("/")[0]
    open_pr = gh_json(
        [
            "api",
            "--method",
            "GET",
            f"repos/{repo.slug}/pulls",
            *repo.api_host,
            "-f",
            "state=open",
            "-f",
            f"head={owner}:{branch}",
        ],
        "[.[] | select(.head.repo.full_name == env.HAX_REPO)] | .[0].number // empty",
        env={"HAX_REPO": repo.slug},
    )
    if open_pr is None:
        refuse("cannot query pull requests")
    if open_pr:
        refuse(
            f"{branch} has open pull request #{open_pr}; "
            f"land it with: gh pr merge --rebase {open_pr}"
        )


def confirm_landing(base: str, head: str, short_target: str, short_head: str) -> None:
    """Landing takes an explicit yes: -y up front, or an answer at the prompt. With no
    terminal to ask and no -y, report what would land and stop, so an unattended caller never
    lands by accident."""
    landing = int(git("rev-list", "--count", f"{base}..{head}"))
    interactive = sys.stdin.isatty()
    verb = "landing" if interactive else "would land"
    print(f"{verb} {landing} commit(s) onto {TARGET}:", file=sys.stderr)
    print(
        git("log", "--reverse", "--oneline", f"--max-count={CONFIRM_LOG_LIMIT}", f"{base}..{head}"),
        file=sys.stderr,
    )
    if landing > CONFIRM_LOG_LIMIT:
        print(f"  ... and {landing - CONFIRM_LOG_LIMIT} older", file=sys.stderr)

    transition = f"{TARGET} {short_target} -> {short_head} once CI is green"
    if not interactive:
        print(f"{transition}; re-run with -y to land", file=sys.stderr)
        sys.exit(NOT_CONFIRMED)

    print(f"{transition}. Proceed? [y/N] ", end="", file=sys.stderr, flush=True)
    try:
        answer = input()
    except EOFError:
        answer = ""
    if answer.strip() not in ("y", "Y", "yes", "YES"):
        print("aborted", file=sys.stderr)
        sys.exit(NOT_CONFIRMED)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=f"Fast-forward {TARGET} onto the current branch once CI is green.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "-t",
        "--timeout",
        type=ci_wait.whole_seconds,
        default=DEFAULT_TIMEOUT_SECONDS,
        metavar="SECONDS",
        help="give up waiting on CI after this long (default: %(default)s)",
    )
    parser.add_argument(
        "-y", "--yes", action="store_true", help="land without asking for confirmation"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    os.chdir(Path(__file__).resolve().parent.parent)

    if shutil.which("gh") is None:
        print(
            f"{paint('error:', ANSI_RED, sys.stderr)} gh not found; install the GitHub CLI",
            file=sys.stderr,
        )
        return 2

    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    if branch == "HEAD":
        refuse("HEAD is detached; land from a branch")
    if branch == TARGET:
        refuse(f"already on {TARGET}")
    if not git_succeeds("diff", "--quiet"):
        refuse("working tree has unstaged changes")
    if not git_succeeds("diff", "--cached", "--quiet"):
        refuse("index has staged changes")

    refuse_if_target_checked_out()

    # Every gh query names this repository rather than letting gh resolve one, so the checks and
    # the push below cannot end up describing different repositories.
    repo = ci_wait.origin_repo()
    if repo is None:
        refuse(f"cannot tell which repository {REMOTE} points at")

    # Landing pushes straight to the target branch, which is a maintainer operation on the
    # canonical repository: a fork's origin is not where the target lives, and read-only access
    # would fail the push at the far end of the CI wait rather than here. Contributors go
    # through a pull request.
    facts = gh_json(
        ["repo", "view", repo.spec, "--json", "isFork,nameWithOwner,viewerPermission"],
        "[.isFork, .nameWithOwner, .viewerPermission] | map(tostring) | join(\" \")",
    )
    if facts is None:
        refuse("cannot query the repository; check that gh is installed and authenticated")
    is_fork, repo_slug, permission = facts.split(" ")
    # The forge spells the repository canonically; a remote URL may not, and the pull request
    # query compares this name against one the forge returns.
    repo = Repo(repo.host, repo_slug)
    if is_fork != "false":
        refuse(f"{repo_slug} is a fork; contribute through a pull request against upstream")
    if permission not in ("ADMIN", "MAINTAIN", "WRITE"):
        refuse(f"no write access to {repo_slug}; contribute through a pull request")

    refuse_if_pull_request_open(branch, repo)

    if not git_succeeds("fetch", "--quiet", REMOTE):
        refuse(f"cannot fetch from {REMOTE}")

    # Every check, the confirmation, and the CI wait speak of this commit rather than HEAD:
    # confirmation can sit open indefinitely, and the branch may gain commits while it does.
    head = git("rev-parse", "HEAD")
    short_head = git("rev-parse", "--short", head)
    local_target = rev_parse(f"refs/heads/{TARGET}")
    if local_target is None:
        refuse(f"no local {TARGET} branch")
    remote_target = rev_parse(f"refs/remotes/{REMOTE}/{TARGET}")
    if remote_target is None:
        refuse(f"{REMOTE} has no {TARGET}")
    remote_branch = rev_parse(f"refs/remotes/{REMOTE}/{branch}")
    if remote_branch is None:
        refuse(f"{REMOTE} has no {branch}; push it first")

    # The branch is the working tree, so a stale or divergent one is the user's to resolve. CI
    # only ever runs against what was pushed, and a green run for the commit presupposes this.
    if remote_branch != head:
        if is_ancestor(head, remote_branch):
            refuse(f"{REMOTE}/{branch} is ahead of {short_head}; pull first")
        elif is_ancestor(remote_branch, head):
            refuse(f"{short_head} is not on {REMOTE}/{branch}; push first")
        else:
            refuse(f"{branch} and {REMOTE}/{branch} have diverged")

    # The remote target is the base the landing is measured against; the local one is never
    # checked out here, and a stale one needs no pull because the update-ref below carries it
    # straight to the landed commit. Commits it has and the remote does not are a different
    # matter: landing would push them along, and they need never have faced CI.
    if local_target != remote_target:
        if is_ancestor(remote_target, local_target):
            refuse(f"local {TARGET} is ahead of {REMOTE}/{TARGET}; push it first")
        elif not is_ancestor(local_target, remote_target):
            refuse(f"{TARGET} and {REMOTE}/{TARGET} have diverged")

    short_target = git("rev-parse", "--short", remote_target)
    if remote_target == head:
        refuse(f"{TARGET} is already at {short_head}; nothing to land")
    if not is_ancestor(remote_target, head):
        refuse(f"{branch} is not a fast-forward of {TARGET}; rebase onto {TARGET} first")

    if not args.yes:
        confirm_landing(remote_target, head, short_target, short_head)

    status = ci_wait.wait_for_run(head, branch, args.timeout, repo)
    if status != ci_wait.Exit.GREEN:
        return status

    # Confirmation can sit open for as long as it likes and the wait itself runs for as long as
    # CI does, so the two conditions that a landing cannot simply overrule are put again here,
    # where a refusal still leaves nothing done. A worktree may have taken the target up in the
    # meantime, this one included, and a pull request may have been opened for the branch.
    refuse_if_target_checked_out()
    refuse_if_pull_request_open(branch, repo)

    # The wait is long enough for every ref checked above to have moved since, HEAD included.
    # Push the commit that was confirmed and went green rather than whatever HEAD now names, and
    # lease the target against the base it was measured from: a plain push would still accept a
    # target that moved to any other ancestor of that commit, a rewind among them.
    push = subprocess.run(
        (
            "git",
            "push",
            "--quiet",
            f"--force-with-lease=refs/heads/{TARGET}:{remote_target}",
            REMOTE,
            f"{head}:refs/heads/{TARGET}",
        ),
        capture_output=True,
        text=True,
    )
    if push.returncode != 0:
        # Only the lease speaks of movement. Credentials, the network, a protected branch and a
        # refusing hook all arrive here too, and each would be slandered by that explanation, so
        # anything else is reported in git's own words.
        if "stale info" in push.stderr:
            refuse(f"push to {REMOTE}/{TARGET} was rejected; it moved during the wait")
        if push.stderr.strip():
            print(push.stderr.strip(), file=sys.stderr)
        refuse(f"push to {REMOTE}/{TARGET} failed")

    # The compare-and-swap rejects every change to the local ref, a change that already put it
    # where this landing wanted it included. Only a ref that came to rest somewhere else is
    # worth saying anything about, and fetching would not move it: the remote is right and the
    # local branch is what drifted.
    if not git_succeeds("update-ref", f"refs/heads/{TARGET}", head, local_target):
        settled = rev_parse(f"refs/heads/{TARGET}")
        if settled != head:
            # Whatever the ref reached is somebody's work and may have moved on again since.
            # Name both ends and leave the choice alone: a bare update-ref here would drop it.
            where = git("rev-parse", "--short", settled) if settled else "no commit"
            refuse(
                f"landed {short_head} on {REMOTE}/{TARGET}, "
                f"but local {TARGET} stands at {where}; reconcile it by hand"
            )

    print(f"{paint('✓ landed', ANSI_GREEN, sys.stdout)} {short_head} on {TARGET}")
    return 0


if __name__ == "__main__":
    # Interleaving with stderr is the whole report's legibility, and a block-buffered
    # stdout would hold a verdict back past the error that followed it.
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(line_buffering=True)
    try:
        status = main()
    except KeyboardInterrupt:
        status = 130
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    except BrokenPipeError:
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, sys.stdout.fileno())
        os.dup2(devnull, sys.stderr.fileno())
    sys.exit(status)
