/* SPDX-License-Identifier: MIT */
#ifndef HAX_FILE_MENTION_H
#define HAX_FILE_MENTION_H

struct input_modal_completer;

/*
 * Interactive file picking for `@` mentions at the prompt.
 *
 * file_mention_pick runs fzf over candidates from `git ls-files`
 * (tracked + untracked, gitignore-respecting) or a pruned `find`
 * outside a repo, with `query` seeding the filter. Absolute / `~/…` /
 * `../…` tokens start the walk at that prefix (selection rejoined under
 * it); in-tree paths stay cwd-relative so a mistyped directory still
 * filters the project list. Returns a malloc'd path, or NULL on cancel /
 * failure — including a selection that is no longer a regular file
 * (warns). fzf is required; without it a one-line notice is printed.
 *
 * Call with the terminal cooked: fzf owns the tty. The line editor's
 * modal-completion handoff guarantees this (input_set_modal_completer).
 */
char *file_mention_pick(const char *query);

/* 1 when fzf is on $PATH. Lets /help dim the shortcut row. */
int file_mention_available(void);

/* `@` policy as the editor's modal Tab completer. match triggers on a
 * token that *starts* with '@' (cursor after it; empty query ok); the
 * replaced span is '@' through the cursor. pick strips the '@' and
 * calls file_mention_pick. */
extern const struct input_modal_completer file_mention_completer;

/* sh -c pipeline builder, exposed for tests. Returns malloc'd. */
char *file_mention_fzf_cmd(const char *query);

#endif /* HAX_FILE_MENTION_H */
