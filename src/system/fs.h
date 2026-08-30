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

/* Return the first whitespace-delimited word of a trusted `sh -c` command line — the program the
 * shell would run — as an allocated string. Returns NULL when that word is absent, or when it
 * contains shell syntax only the shell can evaluate and so cannot be read literally. */
char *fs_shell_head(const char *shell_cmd);

/* Best-effort check that a trusted `sh -c` command line can start: resolve its head like
 * fs_which(). Return 1 when it resolves, or when the head is shell syntax rather than a plain
 * word; 0 otherwise. */
int fs_shell_head_resolves(const char *shell_cmd);

/* Return 0 for a regular file, or -1 with errno set for any other path. */
int ensure_regular_file(const char *path);

/* Open a regular file for reading without blocking on special files. The caller owns the returned
 * descriptor. Returns -1 with errno set on failure or when the path is not a regular file. */
int open_regular_file(const char *path);

/* Return newly allocated, NUL-terminated file contents, or NULL with errno set. */
char *slurp_file(const char *path, size_t *out_len);

/* Read at most cap bytes. On success, optional outputs report the returned length and whether more
 * data exists. The allocation grows with the bytes read rather than cap. Returns NULL with errno
 * set on failure. */
char *slurp_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated);

/* Write exactly length bytes, retrying interrupted and short writes. Returns 0 on success or -1
 * with errno set. */
int write_all(int fd, const void *data, size_t length);

#endif /* HAX_SYSTEM_FS_H */
