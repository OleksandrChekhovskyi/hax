/* SPDX-License-Identifier: MIT */
#ifndef HAX_INPUT_H
#define HAX_INPUT_H

/*
 * Multi-line line editor with in-memory history.
 *
 * Replacement for libedit/readline tailored to a coding-agent REPL:
 *   - Multi-line buffer; redraws across rows on each edit.
 *   - Plain Enter (\r) submits; Shift+Enter (\n) inserts a newline.
 *     Enter on an empty buffer is a no-op (won't submit empty input).
 *     Assumes the terminal sends LF for Shift+Enter — most modern
 *     terminals can be configured this way (iTerm2, kitty, ghostty,
 *     foot, ...).
 *   - Up/Down recall previous/next history; multi-line entries redraw
 *     correctly. Edits to a recalled entry are kept locally; the
 *     in-progress draft is preserved while navigating and restored on
 *     Down past the last entry.
 *   - Ctrl-G opens $EDITOR with the current buffer; on exit the edited
 *     content replaces the buffer. The user keeps editing in the REPL
 *     (no auto-submit).
 *   - Standard motions: arrows, Home/End, Ctrl-A/E/B/F, backspace,
 *     Delete, Ctrl-H/K/U/W, Ctrl-L (clear screen + redraw).
 *   - Bracketed paste is enabled; pasted blocks (incl. newlines) insert
 *     verbatim.
 *   - Ctrl-D on an empty buffer returns NULL (EOF). Ctrl-C cancels the
 *     current line, returning an empty string. Ctrl-Z suspends the
 *     process (job control); editing resumes on `fg`.
 *
 * On a non-tty stdin/stdout, falls back to fgets-style canonical reads
 * (one line per call, no editing) so piped input still works.
 */

struct input;

struct input *input_new(void);
void input_free(struct input *in);

/* Read one message. Returns malloc'd string (caller frees), NULL on EOF
 * (Ctrl-D at empty prompt or non-tty stdin closed). Returns an empty
 * string on Ctrl-C cancellation — the agent loop discards it and re-
 * prompts. The returned string may contain embedded '\n'. */
char *input_readline(struct input *in, const char *prompt);

/* Append `line` to the in-memory history. No-op for NULL/empty input or
 * exact duplicates of the most recent entry. */
void input_history_add(struct input *in, const char *line);

#endif /* HAX_INPUT_H */
