#!/usr/bin/env python3
"""The REPL renders a submitted prompt, the response, and its stats line, then exits 0 on Ctrl-D at
an empty prompt, erasing the prompt row so the resume hint takes its place. The echo drops a
submission's trailing blank lines, and Ctrl+G round-trips the buffer through $EDITOR, keeping it
when the editor fails."""

import harness


def test_prompt_response_exit():
    term = harness.Terminal(
        """
        text Hello from mock
        end-turn
        """,
        extra_env={"HAX_NO_SESSION": "0"},
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
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ hi

        Hello from mock

        {*}s

        resume with: hax --resume={*}
        """
    )


def test_submit_echo_drops_blank_lines():
    term = harness.Terminal(
        """
        text Hello from mock
        end-turn
        """
    )
    term.wait_for("❯")
    term.type("hi")
    term.send("C-j", "C-j", "Enter")
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


# Appends to the buffer it is handed, except that it fails after clobbering one reading "keep".
FAKE_EDITOR = """#!/bin/sh
text=$(cat "$1")
if [ "$text" = keep ]; then
    printf 'clobbered\\n' > "$1"
    exit 1
fi
printf '%s edited\\n' "$text" > "$1"
"""


def test_editor_round_trip():
    editor = harness.scratch_dir() / "editor"
    editor.write_text(FAKE_EDITOR)
    editor.chmod(0o755)
    term = harness.Terminal("end-turn", extra_env={"VISUAL": str(editor), "EDITOR": str(editor)})
    term.wait_for("❯")
    term.type("draft")
    term.send("C-g")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ draft edited
        """
    )
    term.send("C-u")
    term.type("keep")
    term.send("C-g")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ keep
        """
    )


harness.main(globals())
