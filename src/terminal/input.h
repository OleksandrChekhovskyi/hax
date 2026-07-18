/* SPDX-License-Identifier: MIT */
#ifndef HAX_INPUT_H
#define HAX_INPUT_H

#include <stddef.h> /* size_t */

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
 *   - Ctrl-R starts an incremental reverse history search (readline-style
 *     behavior, freshened presentation): the prompt becomes
 *     "reverse-search · query → match" and the buffer tracks the most recent
 *     matching entry as you type. Ctrl-R/Ctrl-S step to older/newer matches,
 *     Backspace shortens the query, Enter accepts and submits, ESC accepts
 *     and keeps editing, Ctrl-G/Ctrl-C abort.
 *   - Ctrl-G opens $EDITOR with the current buffer; on exit the edited
 *     content replaces the buffer. The user keeps editing in the REPL
 *     (no auto-submit).
 *   - Ctrl-T invokes a caller-supplied transcript handler (see
 *     input_set_transcript_cb) — typically pipes a full conversation
 *     view through $PAGER. The buffer is preserved.
 *   - Tab consults a caller-registered modal completer (see
 *     input_set_modal_completer): when its match phase reports a
 *     completable span, its pick phase — typically an interactive file
 *     picker — takes over the terminal and the result replaces the
 *     span. Otherwise Tab inserts a literal tab.
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

/* Append `line` to history. Erasedups semantics: a prior exact match
 * (anywhere in history, not just the most recent) is removed first, so
 * a recalled entry bumps to the top instead of duplicating. No-op for
 * NULL/empty input. When persistence is open (see input_history_open),
 * the entry is also appended to the on-disk file. */
void input_history_add(struct input *in, const char *line);

/* Like input_history_add, but in-memory only — never touches the on-disk
 * file. For ephemeral, session-scoped lines (e.g. slash commands) that are
 * worth recalling with Up-arrow now but pointless to replay in a future,
 * unrelated session. */
void input_history_add_session(struct input *in, const char *line);

/* Enable on-disk history persistence at `path`:
 *   - Loads existing entries (decoded one-line-per-record) into memory,
 *     so Up-arrow recalls them across invocations.
 *   - Stores `path` on `in`; subsequent input_history_add calls append
 *     each accepted entry to the file.
 *   - Creates parent directories as needed; silently no-ops on any I/O
 *     failure (missing $HOME, unwritable dir, etc) — losing history is
 *     never worth crashing the REPL.
 *   - If the on-disk file has grown well past the in-memory cap, the
 *     in-memory state (already capped) is rewritten back atomically. */
void input_history_open(struct input *in, const char *path);

/* Register a Ctrl-T handler. While the prompt is active, pressing Ctrl-T
 * drops the editor out of raw mode, calls `fn(user)`, and repaints.
 * `fn` owns stdout for the duration of the call — the typical
 * implementation popens a pager and pipes content to it. NULL `fn`
 * disables the binding. */
void input_set_transcript_cb(struct input *in, void (*fn)(void *user), void *user);

/* Register a modal Tab completer (full contract on the struct in
 * input_core.h: pure `match` decides whether Tab completes and which
 * span the result replaces; modal `pick` owns the terminal and returns
 * the replacement). The editor stores the pointer, so `mc` must outlive
 * the editor — typically a const static exported by the implementation
 * (see file_mention_completer). NULL unregisters; Tab then always
 * inserts a literal tab. */
struct input_modal_completer;
void input_set_modal_completer(struct input *in, const struct input_modal_completer *mc);

/* Toggle whether Enter on an empty buffer submits the empty string.
 * Off, empty Enter is a no-op; on, input_readline returns "" — used by
 * the REPL while a paused turn is resumable and an empty send means
 * "continue". */
void input_set_empty_submit(struct input *in, int enabled);

/* True when the last input_readline() ended with Ctrl-C. Cancellation
 * also returns "", so a caller that gives an empty submit a meaning
 * (empty_submit above) must check this to keep "discard my typed line"
 * from acting as that meaning. */
int input_cancelled(const struct input *in);

/* Convenience wrapper: open the conventional per-user history file at
 * $XDG_STATE_HOME/hax/history (default $HOME/.local/state/hax/history),
 * but only when stdin and stdout are both ttys — non-interactive use
 * (`echo prompt | hax`) shouldn't leak scripted input into recall
 * history. No-op if neither env var is set. The path-taking variant
 * above remains the testable seam and the hook for a future
 * --history-file override. */
void input_history_open_default(struct input *in);

/* Render `text` (length `len`) as a committed user message — an
 * accent-colored "▌ " stripe (repeated at every wrapped row) and body,
 * word-wrapped at the stripe indent to `term_cols`. Writes directly to
 * stdout and leaves the cursor at column 0 of a fresh row. The editor
 * uses it to repaint a submitted line; history replay (resume) uses it so
 * a restored user message looks byte-for-byte like one just typed. Does
 * not erase any prior content — the caller positions the cursor. */
void input_render_user_message(const char *text, size_t len, int term_cols);

/* The column budget the editor lays user input out within: display_width()
 * (honoring HAX_DISPLAY_WIDTH) clamped to the real tty width. Exposed so a
 * caller passing a width to input_render_user_message (e.g. history replay)
 * can match the editor's wrapping exactly. */
int input_display_cols(void);

#endif /* HAX_INPUT_H */
