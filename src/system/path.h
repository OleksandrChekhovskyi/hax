/* SPDX-License-Identifier: MIT */
#ifndef HAX_PATH_H
#define HAX_PATH_H

/* Filesystem path manipulation. All return malloc'd strings; caller frees.
 * No filesystem I/O — these are textual transforms (no realpath, no
 * normalization of `.`/`..` components). `expand_home` and `collapse_home`
 * do read $HOME from the environment, so they aren't pure functions of
 * their arguments. */

/* Join two filesystem path components with exactly one '/' between them.
 * Strips trailing slashes from `base` (preserving "/" itself as root)
 * and leading slashes from `rel`. Use for path concatenation where
 * either side may have come from an env var (TMPDIR on macOS ends in
 * "/") or a path-walk that lands on root. Both args must be non-NULL;
 * callers passing user/env data should fall back to a literal default
 * before calling. */
char *path_join(const char *base, const char *rel);

/* Expand a leading `~` or `~/` to $HOME. Bare `~` becomes $HOME; `~/x`
 * becomes $HOME/x. Anything else (no tilde, or `~user/`) is returned
 * unchanged. NULL input returns NULL. When $HOME is unset, the original
 * tilde-prefixed input is returned verbatim — caller decides how to
 * surface the resulting "no such file" error. */
char *expand_home(const char *path);

/* Inverse of expand_home for display: replace a leading $HOME prefix
 * with `~`. `$HOME` itself becomes `~`; `$HOME/x` becomes `~/x`.
 * Anything else (no prefix match, $HOME unset, or path matches $HOME
 * only as a non-component substring like "/Users/oleksandr2" against
 * "/Users/oleksandr") is returned verbatim. NULL input returns NULL. */
char *collapse_home(const char *path);

/* If `path` is absolute and lies strictly *under* `cwd` (sharing it as a
 * leading path-component prefix), return the cwd-relative remainder
 * (malloc'd, e.g. "/home/u/proj" + "/home/u/proj/src/x.c" -> "src/x.c").
 * Returns NULL when `path` is relative, equals `cwd`, or sits outside it
 * — leaving the caller to use the original. Like collapse_home, the
 * boundary is component-aware so "/home/u/proj2/x" doesn't match cwd
 * "/home/u/proj". Pure textual transform: no realpath, no symlink
 * resolution, so correctness relies on the caller resolving the result
 * against the same cwd. A path containing a ".." component returns NULL
 * (resolving it could escape cwd, and we don't normalize dot segments).
 * NULL/non-absolute inputs return NULL. */
char *path_relativize(const char *path, const char *cwd);

#endif /* HAX_PATH_H */
