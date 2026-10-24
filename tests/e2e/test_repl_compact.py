#!/usr/bin/env python3
"""Compaction seen from the REPL. A request the model refuses for size is compacted and sent
again: the screen shows the recovery, never the refusal."""

import harness


def test_recovers_an_oversized_request():
    """The refusal is replaced by a notice naming the recovery, and the retried answer follows."""
    term = harness.Terminal(
        """
        error This model's maximum context length is 8192 tokens.
        end-turn

        text ## Goal
        end-turn

        text Answered after compaction.
        end-turn
        """
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.wait_for("Answered after compaction.")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        ── request too large for the context window — compacting ──

        ── conversation compacted ──

        Answered after compaction.

        {*}s

        ❯
        """
    )


harness.main(globals())
