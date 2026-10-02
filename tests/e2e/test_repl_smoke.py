#!/usr/bin/env python3
"""The REPL renders a submitted prompt, the response, and its stats line, then exits 0 on
Ctrl-D at an empty prompt."""

import harness


def test_prompt_response_exit():
    term = harness.Terminal(
        """
        text Hello from mock
        end-turn
        """
    )
    term.wait_for("❯")
    term.type("hi")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ hi

        Hello from mock

        {*}s

        ❯
        """
    )
    term.send("C-d")
    harness.expect(term.wait_exit() == 0, "Ctrl-D on an empty prompt exits 0")


harness.main(globals())
