/* SPDX-License-Identifier: MIT */
#ifndef HAX_FILE_MENTION_H
#define HAX_FILE_MENTION_H

struct input_modal_completer;

/*
 * Interactive file picking for `@` mentions at the prompt.
 *
 * file_mention_pick runs fzf over the project's files (cwd-relative):
 * candidates piped in from `git ls-files` (tracked + untracked,
 * gitignore-respecting) or a pruned `find` outside a repo, with `query`
 * pre-seeding the fuzzy filter. Returns the selected path (malloc'd,
 * cwd-relative) or NULL on cancel, no candidates, or any failure —
 * including a selection that is no longer an existing regular file
 * (stale index entry, deleted mid-pick), which warns and returns NULL.
 * fzf is a hard requirement by design — no built-in fallback matcher;
 * without it a one-line notice is printed and NULL returned.
 *
 * Must be called with the terminal in cooked (non-raw) mode: fzf
 * manages the tty itself. The line editor's modal-completion handoff
 * guarantees this (see input_set_modal_completer).
 */
char *file_mention_pick(const char *query);

/* 1 when fzf is on $PATH — the feature's only external requirement.
 * Lets /help dim the shortcut row instead of advertising a binding
 * that would only print the missing-fzf notice. */
int file_mention_available(void);

/* The `@` policy packaged as the editor's modal Tab completer (register
 * with input_set_modal_completer). match triggers on a token that
 * *starts* with '@' — scan back from the cursor to the nearest
 * whitespace; emails and mid-word '@' don't qualify — with the cursor
 * strictly after the '@' (empty query is fine). The replaced span runs
 * from the '@' through the cursor, so the trigger is consumed by the
 * picked path. pick is file_mention_pick over the token minus its '@'. */
extern const struct input_modal_completer file_mention_completer;

/* Pure command builder for the fzf path, exposed for tests: the `sh -c`
 * pipeline that emits candidates and runs fzf with `query` shell-quoted
 * into --query. Returns malloc'd; caller frees. */
char *file_mention_fzf_cmd(const char *query);

#endif /* HAX_FILE_MENTION_H */
