/* SPDX-License-Identifier: MIT */
#ifndef HAX_INPUT_H
#define HAX_INPUT_H

#include <stddef.h> /* size_t */
#include <stdio.h>  /* FILE */

/* Multi-line terminal editor with history, completion, and modal callback hooks.
 * Non-tty input uses canonical one-line reads without prompts or editing. */

struct input;

struct input *input_new(void);
void input_free(struct input *in);

/* Return a malloc'd message, which may contain '\n'; return NULL on EOF and "" on Ctrl-C. */
char *input_readline(struct input *in, const char *prompt);

/* Add a non-empty line with erasedups semantics. When persistence is open, append it to
 * disk as well. */
void input_history_add(struct input *in, const char *line);

/* Add a non-empty line to in-memory history without writing it to disk. */
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

/* Register a modal Tab completer. The editor borrows `completer`, which must outlive it.
 * NULL unregisters the completer and makes Tab insert a literal tab. */
struct input_modal_completer;
void input_set_modal_completer(struct input *in, const struct input_modal_completer *completer);

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

/* Distinguish Ctrl-C from an enabled empty submission; both return "". */
int input_cancelled(const struct input *in);

/* Load the conventional XDG history file only for tty sessions. `persist` controls whether
 * later entries are appended; scripted input is never retained. */
void input_history_open_default(struct input *in, int persist);

/* Write a committed user message with an accent stripe on each row, wrapped to
 * `display_columns`. Leave the cursor at column 0 of a fresh row without erasing prior content. */
void input_render_user_message_to(FILE *out, const char *text, size_t len, int display_columns);

/* Return display_width() clamped to the current terminal width. */
int input_display_cols(void);

#endif /* HAX_INPUT_H */
