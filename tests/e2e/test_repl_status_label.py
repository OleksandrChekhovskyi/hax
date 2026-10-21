#!/usr/bin/env python3
"""The status row's label while a turn waits on something silent: it says what the wait is for,
once the wait outlasts the label's settle time, and gives way once the wait ends."""

import os

import harness


def test_says_composing_while_a_table_streams():
    """A table appears only once complete, so a long one is labeled rather than looking stalled,
    even while its rows keep arriving."""
    # Rows arrive well inside the label's settle time and keep coming past it; the scenario ends
    # once the label shows, long before the table would.
    rows = "".join(f"text | row {i} | {i} |\\n\ndelay 100\n" for i in range(100))
    term = harness.Terminal(
        "text Here is a table:\\n\\n| Name | Value |\\n|---|---|\\n\n" + rows + "end-turn\n",
        extra_env={"HAX_LABEL_SETTLE_MS": "500"},
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.wait_for("composing...")


def test_running_tool_label_gives_way_when_the_model_resumes():
    """A finished tool's label does not linger into the model's next response."""
    term = harness.Terminal(
        """
        tool bash {"command":"cat fifo"}
        end-turn
        delay 10000
        text Done.
        end-turn
        """,
        extra_env={"HAX_LABEL_SETTLE_MS": "1000"},
    )
    os.mkfifo(term.workdir / "fifo")
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.wait_for("[bash] running...")
    term.write_fifo(term.workdir / "fifo", "done\n")
    # Half the settle time, so a label that gave way only after settling would fail this.
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        [bash] cat fifo

        {*} working...
        """,
        timeout=0.5,
    )


if __name__ == "__main__":
    harness.main(globals())
