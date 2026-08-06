/* SPDX-License-Identifier: MIT */
#ifndef HAX_FILE_MENTION_H
#define HAX_FILE_MENTION_H

struct input_modal_completer;

/* Open fzf with `query_text` as its initial filter. Absolute, home-relative, and parent-relative
 * queries search from their directory prefix; other queries search the current directory. The
 * terminal must be in cooked mode. Return an allocated regular-file path, or NULL on cancellation
 * or failure. A NULL query is equivalent to an empty query. */
char *file_mention_pick(const char *query_text);

/* Return whether fzf is currently available on PATH. */
int file_mention_available(void);

/* Match tokens beginning with `@` at the buffer start or after whitespace, replacing from `@`
 * through the cursor. */
extern const struct input_modal_completer file_mention_completer;

/* Return an allocated /bin/sh command for the given query; NULL is equivalent to an empty query. */
char *file_mention_build_fzf_command(const char *query_text);

#endif /* HAX_FILE_MENTION_H */
