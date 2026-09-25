/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_INPUT_H
#define HAX_TERMINAL_INPUT_H

#include <stddef.h>
#include <stdio.h>

/* Multi-line terminal editor with history, completion, and modal callback hooks.
 * Non-tty input uses canonical one-line reads without prompts or editing. */

struct input;

struct input *input_new(void);
void input_free(struct input *in);

enum input_readline_result {
    INPUT_READLINE_LINE,
    INPUT_READLINE_EOF,
    INPUT_READLINE_TIMEOUT,
    INPUT_READLINE_CANCELLED,
};

/* Read one editor line. A nonzero deadline is an absolute monotonic instant; when it passes,
 * `line_out` receives the current draft and INPUT_READLINE_TIMEOUT is returned. A bare Escape
 * returns INPUT_READLINE_CANCELLED under the same deadline. The returned text is malloc'd. */
enum input_readline_result input_readline_until(struct input *in, const char *prompt,
                                                long deadline_ms, char **line_out);

/* Return a malloc'd message, which may contain '\n'; return NULL on EOF or a
 * double Ctrl-C quit. Ctrl-C with text clears the buffer and editing continues. */
char *input_readline(struct input *in, const char *prompt);

/* Add a non-empty line with erasedups semantics. When persistence is open, append it to
 * disk as well. */
void input_history_add(struct input *in, const char *line);

/* Add a non-empty line to in-memory history without writing it to disk. A later
 * input_history_add of the same line still persists it. */
void input_history_add_session(struct input *in, const char *line);

/* Load history from `path` and append later input_history_add entries there. Create parent
 * directories as needed and ignore I/O failures. Oversized files are atomically compacted to the
 * retained in-memory entries. */
void input_history_open(struct input *in, const char *path);

/* Load `path` for recall without creating, rewriting, or appending to the file.
 * Subsequent input_history_add calls remain in-memory only. */
void input_history_load(struct input *in, const char *path);

/* Control-byte value for Ctrl-<c>, for naming a binding at the call site
 * (INPUT_KEY_CTRL('O') rather than 0x0f). */
#define INPUT_KEY_CTRL(c) ((unsigned char)((c) & 0x1f))

/* Bind a control byte to a cooked-mode callback that owns the terminal until it returns.
 * Built-in editing keys take precedence. Rebinding replaces the callback and NULL clears it.
 * Return -1 for printable keys or when all slots are occupied. */
int input_bind_modal_key(struct input *in, unsigned char key, void (*fn)(void *user), void *user);

/* Register a Tab completer. The editor borrows `completer`, which must outlive it. Tab tries
 * completers in registration order and does nothing when none matches; it never inserts a
 * literal tab. A second Tab that still has nothing to add shows the completer's candidates as
 * ghost text. Ghost text is painted dim after the buffer, so only while the cursor is at its end,
 * and lasts until the next key. Return -1 when all slots are occupied. */
struct input_completer;
int input_add_completer(struct input *in, const struct input_completer *completer);

/* Register the ghost-text hint: after each edit `fn` receives the buffer and returns a malloc'd
 * string, or NULL for no hint. The hook must not touch the tty; NULL `fn` disables it. */
void input_set_hint(struct input *in, char *(*fn)(const char *buf, void *user), void *user);

/* Register the Ctrl-V paste hook. Ctrl-V or an empty bracketed paste calls
 * `fn(user)` and inserts the returned malloc'd string at the cursor, freeing it.
 * NULL return inserts nothing; NULL `fn` disables the binding. The hook
 * must not touch the tty — the editor stays in raw mode around the
 * call. */
void input_set_paste_hook(struct input *in, char *(*fn)(void *user), void *user);

/* Register the bracketed-paste body filter. `fn` receives each non-empty,
 * NUL-terminated body after CR/CRLF normalization and NUL-byte replacement. It returns a
 * malloc'd replacement, which the editor frees, or NULL to insert the body verbatim. The hook
 * must not touch the tty; NULL disables it. */
void input_set_paste_filter(struct input *in, char *(*fn)(const char *text, void *user),
                            void *user);

/* Seed the next editable input with `text`, placing the cursor at the end.
 * One-shot; NULL or "" clears it, and non-tty reads discard it. */
void input_set_preseed(struct input *in, const char *text);

/* When enabled, Enter on an empty buffer returns "" instead of doing nothing. */
void input_set_empty_submit(struct input *in, int enabled);

/* Load `path` only for tty sessions; `persist` controls whether later entries are appended
 * there. Scripted input is never retained, so non-tty sessions leave `path` untouched. */
void input_history_open_tty(struct input *in, const char *path, int persist);

/* Write a committed user message with an accent stripe on each row, wrapped to
 * `display_columns`. Leave the cursor at column 0 of a fresh row without erasing prior content. */
void input_render_user_message_to(FILE *out, const char *text, size_t len, int display_columns);

/* Return display_width() clamped to the current terminal width. */
int input_display_cols(void);

#endif /* HAX_TERMINAL_INPUT_H */
