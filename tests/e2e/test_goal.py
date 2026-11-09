#!/usr/bin/env python3
"""Goal command submission, cancellation, and persistence across REPL runs."""

import json

import harness


def test_goal_command_submits_and_finishes():
    term = harness.Terminal(
        """
        text Working on it.
        end-turn
        text Verified the result.\\n[goal complete]
        end-turn
        """,
        size=(100, 32),
    )
    term.wait_for("❯")
    term.type("/goal fix tests")
    term.send("Enter")
    term.wait_for("Verified the result.")
    term.wait_for("❯")
    term.type("/goal")
    term.send("Enter")
    term.wait_for("no active goal")


def test_goal_clear_disables_continuation():
    term = harness.Terminal(
        """
        text Progress before the limit.
        end-turn
        text Ordinary answer after clearing.
        end-turn
        """,
        extra_env={"HAX_MAX_TURNS": "1"},
        size=(100, 32),
    )
    term.wait_for("❯")
    term.type("/goal fix tests")
    term.send("Enter")
    term.wait_for("max turns reached — enter to continue")
    term.type("/goal clear")
    term.send("Enter")
    term.wait_for("goal cleared")
    term.type("respond normally")
    term.send("Enter")
    term.wait_for("Ordinary answer after clearing.")
    term.wait_for("❯")
    harness.expect("mock script exhausted" not in term.screen(), "clearing stops goal retries")


def test_goal_survives_session_resume():
    home, workdir = harness.make_home()
    term = harness.Terminal(
        """
        text Recorded goal progress.
        end-turn
        """,
        home=home,
        workdir=workdir,
        extra_env={"HAX_NO_SESSION": "0", "HAX_MAX_TURNS": "1"},
    )
    term.wait_for("❯")
    term.type("/goal fix tests")
    term.send("Enter")
    term.wait_for("max turns reached — enter to continue")
    term.send("C-d")
    term.wait_exit()
    sessions = list((home / ".local/state/hax/sessions").glob("*/*.jsonl"))
    harness.expect(len(sessions) == 1, "goal records one session")
    records = [json.loads(line) for line in sessions[0].read_text().splitlines()]
    harness.expect(any(record.get("origin") == "goal" for record in records), "goal is recorded")
    resumed = harness.Terminal(
        """
        text Goal resumed and verified.\\n[goal complete]
        end-turn
        """,
        args=[f"--resume={records[0]['id']}"],
        home=home,
        workdir=workdir,
        extra_env={"HAX_NO_SESSION": "0"},
        size=(100, 32),
    )
    resumed.wait_for("❯")
    resumed.type("/goal")
    resumed.send("Enter")
    resumed.wait_for("goal: fix tests")
    resumed.send("Enter")
    resumed.wait_for("Goal resumed and verified.")
    resumed.wait_for("❯")
    resumed.type("/goal")
    resumed.send("Enter")
    resumed.wait_for("no active goal")


harness.main(globals())
