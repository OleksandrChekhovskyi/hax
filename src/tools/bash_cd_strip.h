/* SPDX-License-Identifier: MIT */
#ifndef HAX_BASH_CD_STRIP_H
#define HAX_BASH_CD_STRIP_H

#include <stddef.h>

/* If `cmd` starts with `cd <target> && ...` where <target> resolves to
 * `cwd` after shell quoting and tilde / $HOME expansion, return the
 * byte offset of the first character past that prefix. Otherwise
 * return 0 (leave the command untouched).
 *
 * Pure, no I/O — cwd and home are passed explicitly so tests can pin
 * them. cwd must be the absolute, normalized path the agent runs from
 * (typical getcwd() output); home may be NULL when $HOME is unset.
 *
 * The motivating case is Qwen-family models prepending `cd <project>
 * && ` to every bash call. We strip when the cd is a filesystem
 * no-op — i.e. the target resolves to cwd and the substitution can't
 * be reshaped by word splitting or pathname expansion — so the
 * working directory and exit code stay the same.
 *
 * Conservative on anything we don't model exactly (command
 * substitution, backslash escapes, ~user, unrecognized variables,
 * mixed concatenated quoting): returns 0 rather than risking a
 * wrong rewrite. */
size_t bash_strip_cd_prefix(const char *cmd, const char *cwd, const char *home);

#endif /* HAX_BASH_CD_STRIP_H */
