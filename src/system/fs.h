/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_FS_H
#define HAX_SYSTEM_FS_H

#include <stddef.h>

/* Create `path` and missing parent directories with mode 0755, subject to the process umask.
 * Existing directories and symlinks to directories are accepted. NULL and empty paths are no-ops.
 * Returns 0 on success and -1 with errno set on failure. */
int fs_mkdir_p(const char *path);

/* Resolve a symlink chain without requiring the final target to exist. Relative targets are
 * resolved from the containing link's directory. Returns an allocated path, or NULL with errno
 * set when resolution fails or exceeds the symlink-hop limit. */
char *fs_resolve_link_target(const char *path);

/* Atomically replace `path` and return an allocated unified diff. Existing file modes are
 * preserved, missing parent directories are created, and unchanged files retain their inode.
 * Returns NULL on failure and stores an allocated explanation in `*error`. When non-NULL,
 * `*was_created` reports whether the successful write created a file rather than replacing one. */
char *fs_write_with_diff(const char *path, const char *content, size_t content_len, char **error,
                         int *was_created);

/* Resolve `name` against PATH and return the first executable regular file as an allocated path.
 * Names containing '/' are checked directly. Empty and relative PATH entries are deliberately
 * ignored so lookup cannot select a program from the process's current directory. */
char *fs_which(const char *name);

/* Best-effort check that a trusted `sh -c` command line can start: resolve its first
 * whitespace-delimited word like fs_which(). Return 1 when it resolves, or when the first word
 * contains shell syntax only the shell can evaluate; 0 otherwise. */
int fs_shell_head_resolves(const char *shell_cmd);

#endif /* HAX_SYSTEM_FS_H */
