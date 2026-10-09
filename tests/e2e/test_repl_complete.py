#!/usr/bin/env python3
"""Tab completion at the REPL prompt. The first Tab extends the word at the cursor as far as it is
unambiguous; a second Tab lists the candidates as dim text after the cursor, which the next key
clears. Which words complete to what is pinned by the slash unit tests; these check the keys and
the painting."""

import json

import harness


def test_command_name():
    """An ambiguous name extends to the shared prefix, and a completed one shows its arguments."""
    term = harness.Terminal("")
    term.wait_for("❯")
    term.type("/pre")
    term.send("Tab")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ /preset [name]
        """
    )

    term.send("Tab")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ /preset  /preset /preset-save
        """
    )
    harness.expect(
        "\x1b[2m  /preset /preset-save" in term.screen(styles=True),
        "the candidates are dim, unlike typed text",
    )

    term.type("-")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ /preset-
        """
    )
    term.send("Tab")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ /preset-save <name> [tint]
        """
    )


def test_argument_submits():
    """A completed argument leaves a trailing space, and the line still runs as typed in full."""
    home, workdir = harness.make_home()
    config_dir = home / ".config" / "hax"
    config_dir.mkdir(parents=True)
    preset = {"provider": "mock", "model": "mock-model"}
    (config_dir / "config.json").write_text(
        json.dumps({"presets": {"fast": preset, "focus": preset}})
    )
    term = harness.Terminal("", home=home, workdir=workdir)
    term.wait_for("❯")
    term.type("/preset f")
    term.send("Tab", "Tab")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ /preset f  fast focus
        """
    )

    term.type("o")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ /preset fo
        """
    )
    term.send("Tab")
    term.wait_for("❯ /preset focus")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ /preset focus

        ▌ hax [focus] › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯
        """
    )


harness.main(globals())
