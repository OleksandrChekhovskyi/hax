#!/usr/bin/env python3
"""The status row while a turn is in progress. It stays up as output streams, one blank row below
what has been written, and repainting it leaves that output as it was. Tool calls are in
test_repl_status_tools.py, and what its label says is in test_repl_status_label.py."""

import harness


def test_stays_below_streaming_text():
    """A pause in the middle of an answer leaves the status row up below the partial text."""
    term = harness.Terminal(
        """
        text The first part of an answer, streamed before a pause.
        delay 30000
        text Never delivered.
        end-turn
        """
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        The first part of an answer, streamed before a pause.

        {*} working...
        """
    )


def test_full_width_row_keeps_its_place():
    """Text that exactly fills a row leaves the next write to wrap below it, not over it."""
    term = harness.Terminal(
        "text ```\\n" + "A" * 80 + "\ndelay 200\ntext MARKER\\n```\\n\nend-turn\n"
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        """
        + "A" * 80
        + """
        MARKER

        {*}s

        ❯
        """
    )


def test_output_keeps_its_style_across_chunks():
    """Styling open when the status row repaints carries on into the next chunk."""
    # The status row is up from the start of the stream and repaints after every chunk.
    term = harness.Terminal(
        """
        text Here is **bold
        text , and its end** after.
        reasoning First thought
        reasoning , then a second.
        delay 30000
        end-turn
        """,
        extra_env={"HAX_SHOW_REASONING": "1"},
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.wait_for("then a second.")
    screen = term.screen(styles=True)
    harness.expect(
        "\x1b[1mbold, and its end\x1b[0m" in screen, "bold spans both chunks"
    )
    harness.expect(
        "\x1b[2;3mFirst thought, then a second." in screen, "reasoning stays dim and italic"
    )


if __name__ == "__main__":
    harness.main(globals())
