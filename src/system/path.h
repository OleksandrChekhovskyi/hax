/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_PATH_H
#define HAX_SYSTEM_PATH_H

/* Textual path transforms: no filesystem access or dot-segment normalization. Non-NULL results are
 * allocated and owned by the caller. */

/* Join non-NULL paths with one separator, trimming trailing slashes from base (except root) and
 * leading slashes from suffix. */
char *path_join(const char *base, const char *suffix);

/* Expand bare `~` and a leading `~/` using non-empty $HOME. Other inputs are copied unchanged;
 * NULL returns NULL. */
char *path_expand_home(const char *path);

/* Replace a leading, component-aligned non-empty $HOME with `~`. Other inputs are copied unchanged;
 * NULL returns NULL. */
char *path_collapse_home(const char *path);

/* Return the portion of absolute path lexically beneath absolute cwd. Returns NULL for invalid or
 * unrelated paths, equality, and paths containing a `..` component. */
char *path_relativize(const char *path, const char *cwd);

#endif /* HAX_SYSTEM_PATH_H */
