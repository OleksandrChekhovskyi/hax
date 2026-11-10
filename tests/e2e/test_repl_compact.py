#!/usr/bin/env python3
"""Automatic compaction from the REPL. A reply that crosses the threshold is not summarized when
it ends; the next send compacts first, then answers from the compacted context."""

import harness


def test_compacts_before_next_send():
    """The over-threshold reply ends at the prompt; the summary runs only once a new message
    is sent, ahead of its answer."""
    term = harness.Terminal(
        """
        text First answer.
        usage in=900 out=50
        end-turn

        text ## Goal\\n- carry on
        end-turn

        text Second answer.
        usage in=100 out=10
        end-turn
        """,
        extra_env={"HAX_CONTEXT_LIMIT": "1000"},
    )
    term.wait_for("❯")
    term.type("first")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ first

        First answer.

        {*}s · 950 / 1k (95%)

        ❯
        """
    )

    term.type("second")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ first

        First answer.

        {*}s · 950 / 1k (95%)

        ▌ second

        ── conversation compacted ──

        Second answer.

        {*}s · 110 / 1k (11%)

        ❯
        """
    )


def test_esc_returns_the_unsent_message():
    """An Esc during that compaction stops the send and hands the message back to the prompt."""
    term = harness.Terminal(
        """
        text First answer.
        usage in=900 out=50
        end-turn

        # The summary stalls before its first token, so the Esc lands before it starts; the
        # stall ends as soon as the Esc cuts the request.
        delay 10000
        text ## Goal
        end-turn
        """,
        extra_env={"HAX_CONTEXT_LIMIT": "1000"},
    )
    term.wait_for("❯")
    term.type("first")
    term.send("Enter")
    term.wait_for("950 / 1k (95%)")
    term.type("second")
    term.send("Enter")
    term.wait_for("compacting...")
    term.send("Escape")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ first

        First answer.

        {*}s · 950 / 1k (95%)

        ▌ second

        ── compaction cancelled ──

        message not sent — press Enter to send it

        ❯ second
        """
    )


harness.main(globals())
