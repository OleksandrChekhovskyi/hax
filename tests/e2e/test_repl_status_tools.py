#!/usr/bin/env python3
"""The status row while tool calls run: below a collapsed call's row, an open read row, and a
verbose call's live output, with the output settling around it."""

import os

import harness


def test_stays_up_while_a_collapsed_tool_runs():
    """A quiet tool still shows that the turn is busy while it runs."""
    term = harness.Terminal(
        """
        tool bash {"command":"cat fifo"}
        end-turn
        """
    )
    os.mkfifo(term.workdir / "fifo")
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] cat fifo

        {*}...
        """
    )


def test_stays_below_a_running_tools_output():
    """A verbose tool's newest line stays open above the status row, even once a blank line fills
    the head preview, and settles closed when the tool finishes."""
    term = harness.Terminal(
        """
        tool bash {"command":"seq 2; read -r a < fifo; seq 3 4; echo; read -r a < fifo"}
        end-turn
        text Done.
        end-turn
        """
    )
    os.mkfifo(term.workdir / "fifo")
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] seq 2; read -r a < fifo; seq 3 4; echo; read -r a < fifo
        ┌ 1
        ╵ 2

        {*}...
        """
    )
    # A blank line fills the head preview, but nothing newer replaces 4 yet.
    term.write_fifo(term.workdir / "fifo", "next\n")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] seq 2; read -r a < fifo; seq 3 4; echo; read -r a < fifo
        ┌ 1
        │ 2
        │ 3
        ╵ 4

        {*}...
        """
    )
    term.write_fifo(term.workdir / "fifo", "next\n")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] seq 2; read -r a < fifo; seq 3 4; echo; read -r a < fifo
        ┌ 1
        │ 2
        │ 3
        └ 4

        Done.

        {*}s

        ❯
        """
    )


def test_coalesces_reads_around_the_status_row():
    """A read row left open for the next read keeps the status row below it, and the next read
    extends the row in place."""
    # The cluster stays open across turns, so the second read can arrive after a visible wait.
    term = harness.Terminal(
        """
        tool read {"path":"a.txt"}
        end-turn
        delay 250
        tool read {"path":"b.txt"}
        delay 0
        end-turn
        text Done.
        end-turn
        """
    )
    (term.workdir / "a.txt").write_text("a\n")
    (term.workdir / "b.txt").write_text("b\n")
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [read] a.txt

        {*}...
        """
    )
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [read] a.txt, b.txt

        Done.

        {*}s

        ❯
        """
    )


if __name__ == "__main__":
    harness.main(globals())
