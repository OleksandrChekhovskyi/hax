/* SPDX-License-Identifier: MIT */
#ifndef HAX_UI_H
#define HAX_UI_H

/*
 * User-facing status lines for the interactive REPL surface.
 *
 * These own the "status line in the terminal" convention in one place:
 * they pick the stream (stdout, so the message interleaves correctly with
 * the rest of the on-screen conversation), gate the color on stdout being
 * a TTY (no escape bytes when piped or redirected), and append the
 * trailing newline. Callers pass a printf-style message with no color
 * codes and no newline.
 *
 *   ui_error — red; something went wrong (bad command, /usage failed, …).
 *   ui_note  — dim; an informational aside (copied to clipboard, nothing
 *              to resume, feature unsupported by this provider, …).
 *
 * Scope is deliberately narrow — the conversational/slash-command surface.
 * Neither is for:
 *   - startup / CLI errors, which print "hax: ..." to stderr before the
 *     REPL exists (see hax_err() in util.h) so scripts can parse them;
 *   - mid-stream provider errors, which flow through the unified EV_ERROR
 *     event and are rendered by the agent's display layer.
 */
__attribute__((format(printf, 1, 2))) void ui_error(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void ui_note(const char *fmt, ...);

#endif /* HAX_UI_H */
