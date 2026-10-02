#!/usr/bin/env python3
"""Stopping a turn from the REPL. A soft interrupt (Esc) pauses at the next turn seam and is named
only by a dim prompt placeholder; a hard interrupt (Esc twice) ends the user turn with an
[interrupted] line above the stats line. Either way, an empty Enter continues."""

import os
import time

import harness

# The mock provider defaults to an unrecorded session; resuming needs the file.
RECORD = {"HAX_NO_SESSION": "0"}


def submit(mock_script: str, fifo: bool = False, **terminal_args) -> harness.Terminal:
    """Start hax on `mock_script`, with ./fifo for scripts that block on it, and submit a prompt."""
    term = harness.Terminal(mock_script, **terminal_args)
    if fifo:
        os.mkfifo(term.workdir / "fifo")
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    return term


def paused() -> harness.Terminal:
    """Pause a turn while its tool runs, and wait for the pause to land at the turn's end."""
    term = submit(
        """
        tool bash {"command":"cat fifo"}
        end-turn

        text Resumed where it stopped.
        end-turn
        """,
        fifo=True,
    )
    term.wait_for("[bash] cat fifo")
    term.send("Escape")
    # Nothing on screen acknowledges an Esc during a tool run, and it must land before the tool
    # finishes, or the turn completes instead of pausing.
    time.sleep(0.3)
    term.write_fifo(term.workdir / "fifo", "unblocked\n")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] cat fifo

        {*}s

        ❯ paused — enter to continue
        """
    )
    return term


def interrupt(
    mock_script: str, ready: str, fifo: bool = False, **terminal_args
) -> harness.Terminal:
    """Submit a prompt, wait for `ready` on screen, and interrupt there."""
    term = submit(mock_script, fifo, **terminal_args)
    term.wait_for(ready)
    # The first Esc requests a pause, the second upgrades it to an abort.
    term.send("Escape", "Escape")
    return term


def test_soft_placeholder():
    """The placeholder is dim, typing replaces it, and slash-command output does not hide it."""
    term = paused()
    harness.expect(
        "\x1b[2mpaused — enter to continue" in term.screen(styles=True),
        "the placeholder is dim, unlike typed text",
    )

    term.type("x")
    term.wait_for("❯ x")
    harness.expect("enter to continue" not in term.screen(), "typing replaces the placeholder")
    term.send("BSpace")
    term.wait_for("❯ paused — enter to continue")

    term.type("/effort")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] cat fifo

        {*}s

        ▌ /effort

        the mock provider doesn't expose reasoning-effort levels

        ❯ paused — enter to continue
        """
    )


def test_soft_empty_enter_resumes():
    """The resumed output takes the prompt row's place, one blank row below the stats line."""
    term = paused()
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] cat fifo

        {*}s

        Resumed where it stopped.

        {*}s

        ❯
        """
    )


def test_hard_mid_text():
    """The stream stalls after its first sentence."""
    interrupt(
        """
        text Partial answer before the interrupt.
        delay 30000
        text Never delivered.
        end-turn
        """,
        ready="Partial answer",
    ).expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        Partial answer before the interrupt.

        [interrupted]

        {*}s

        ❯ enter to continue
        """
    )


def test_hard_killed_tool():
    """A verbose tool is killed mid-run and marks its own call as well."""
    interrupt(
        """
        text Running a slow command.
        tool bash {"command":"sleep 30"}
        end-turn
        """,
        ready="[bash] sleep 30",
    ).expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        Running a slow command.

        [bash] sleep 30
        › [interrupted]

        [interrupted]

        {*}s

        ❯ enter to continue
        """
    )


def test_hard_collapsed_tool():
    """A collapsed tool hides its result, so only the turn's marker shows the interrupt."""
    interrupt(
        """
        tool bash {"command":"cat fifo"}
        end-turn
        """,
        ready="[bash] cat fifo",
        fifo=True,
    ).expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] cat fifo

        [interrupted]

        {*}s

        ❯ enter to continue
        """
    )


def test_hard_undispatched_call():
    """The call streams right after the text but is never dispatched, so nothing draws it."""
    interrupt(
        """
        text About to run.
        tool bash {"command":"echo hi"}
        delay 30000
        text Never delivered.
        end-turn
        """,
        ready="About to run.",
    ).expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        About to run.

        [interrupted]

        {*}s

        ❯ enter to continue
        """
    )


def test_hard_continue_replays():
    """--continue replays an interrupted conversation as the live run showed it."""
    script = """
        text Partial answer before the interrupt.
        delay 30000
        text Never delivered.
        end-turn
    """
    home, workdir = harness.make_home()
    live = interrupt(script, ready="Partial answer", home=home, workdir=workdir, extra_env=RECORD)
    live.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        Partial answer before the interrupt.

        [interrupted]

        {*}s

        ❯ enter to continue
        """
    )
    live.send("C-d")
    harness.expect(live.wait_exit() == 0, "the interrupted run exits 0 on Ctrl-D")

    resumed = harness.Terminal(script, ["--continue"], home=home, workdir=workdir, extra_env=RECORD)
    resumed.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ── resumed · ctrl-o for full history ──

        ▌ go

        Partial answer before the interrupt.

        [interrupted]

        {*}s

        ❯ enter to continue
        """
    )


harness.main(globals())
