#!/usr/bin/env python3
"""Terminal notifications from the REPL: status reports, the focus-gated bell, and both across a
Ctrl-Z stop."""

import harness

FOCUS_OUT = "\x1b[O"
WORKING = b"\x1b]7501;state=working:app=hax\x1b\\"
DONE = b"\x1b]7501;state=done:app=hax\x1b\\"
IDLE = b"\x1b]7501;state=idle:app=hax\x1b\\"
CLEAR = b"\x1b]7501;state=clear\x1b\\"
BEL = b"\x07"
REPORTING_ON = b"\x1b[?1004h"
REPORTING_OFF = b"\x1b[?1004l"


def test_focus_gates_the_bell():
    term = harness.Terminal(
        """
        text First answer
        end-turn

        text Second answer
        end-turn
        """
    )
    term.wait_for("❯")
    term.record_output()

    term.type("go")
    term.send("Enter")
    # Input sent before the next prompt could be discarded with what arrived during the turn.
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        First answer

        {*}s

        ❯
        """
    )

    term.type(FOCUS_OUT)
    term.type("again")
    output = term.wait_output(IDLE)
    first_answer_end = output.index(DONE) + len(DONE)
    harness.expect(
        output[first_answer_end : first_answer_end + 1] != BEL,
        "no bell before the terminal reports a focus change",
    )
    harness.expect(
        output.index(DONE) < output.index(IDLE), "the first key after an answer acknowledges it"
    )
    term.send("Enter")
    term.wait_output(DONE + BEL)
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        First answer

        {*}s

        ▌ again

        Second answer

        {*}s

        ❯
        """
    )

    term.send("C-d")
    harness.expect(term.wait_exit() == 0, "Ctrl-D exits 0")
    output = term.wait_output(CLEAR)
    harness.expect(output.count(WORKING) == 2, "each prompt reports working once")
    harness.expect(
        output.rindex(REPORTING_OFF) > output.rindex(REPORTING_ON),
        "focus reporting is off once hax exits",
    )


def test_stop_hands_back_the_terminal():
    term = harness.Terminal(
        """
        text Working
        delay 1000
        text  done.
        end-turn
        """
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.wait_for("Working")
    term.record_output()
    # The pane's process group is orphaned, so the kernel skips the stop itself and hax resumes
    # at once, but the handler still runs both halves.
    term.send("C-z")
    output = term.wait_output(DONE)
    off, clear, on = output.find(REPORTING_OFF), output.find(CLEAR), output.find(REPORTING_ON)
    harness.expect(0 <= off < clear < on, "a stop turns focus reporting and the status off")
    harness.expect(on < output.find(WORKING) < output.index(DONE), "both return on resume")


harness.main(globals())
