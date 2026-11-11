#!/usr/bin/env python3
"""Automatic compaction from the REPL. A reply that crosses the threshold is not summarized when
it ends; the next send compacts first, then answers from the compacted context. A request the
provider rejects as too large is compacted and sent again."""

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


# A follow-up request rejected for size, then the summary; the turn after it answers the retry.
OVERFLOW_THEN_SUMMARY = """
    tool bash {"command":"seq 1 4000"}
    end-turn

    # Requests over the limit, such as the one carrying the tool output, fail and replay this
    # turn; the summary request, its output clipped, fits and gets the summary.
    overflow limit=3000
    text ## Goal\\n- count
    end-turn
    """

TOOL_OUTPUT_ROWS = """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ count

        [bash] seq 1 4000
        ┌ 1
        │ 2
        │ 3
        │ 4
        │ ... (3992 more lines) ...
        │ 3997
        │ 3998
        │ 3999
        └ 4000

        ── conversation compacted ──
        """


def test_rejected_request_is_compacted_and_retried():
    """The rejection itself never shows: the conversation is compacted and the answer follows."""
    term = harness.Terminal(OVERFLOW_THEN_SUMMARY + "text Counted.\n")
    term.wait_for("❯")
    term.type("count")
    term.send("Enter")
    term.expect_screen(
        TOOL_OUTPUT_ROWS
        + """
        Counted.

        {*}s

        ❯
        """
    )


def test_partial_reply_is_replaced():
    """A reply cut off by the full context stays on screen above the notice, and the answer that
    follows starts over."""
    term = harness.Terminal(
        """
        # The context fills partway through the reply.
        text The answer so
        overflow
        end-turn

        # The summary.
        text ## Goal\\n- answer
        end-turn

        # The retry over the seed.
        text The full answer.
        end-turn
        """
    )
    term.wait_for("❯")
    term.type("explain")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ explain

        The answer so

        ── conversation compacted ──

        The full answer.

        {*}s

        ❯
        """
    )


def test_rejection_without_progress_is_reported():
    """When the first batch after compacting is rejected too, compacting again would likely end
    the same way, so its error ends the turn."""
    term = harness.Terminal(
        OVERFLOW_THEN_SUMMARY
        + """
        tool bash {"command":"seq 1 4000"}
        end-turn

        overflow
        """,
        size=(80, 40),
    )
    term.wait_for("❯")
    term.type("count")
    term.send("Enter")
    term.expect_screen(
        TOOL_OUTPUT_ROWS
        + """
        [bash] seq 1 4000
        ┌ 1
        │ 2
        │ 3
        │ 4
        │ ... (3992 more lines) ...
        │ 3997
        │ 3998
        │ 3999
        └ 4000

        [error: the request exceeds the available context size, try increasing it]

        ❯ enter to retry
        """
    )


harness.main(globals())
