/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_TEMPFILES_H
#define HAX_SYSTEM_TEMPFILES_H

/* Process-wide registry for temporary files referenced by conversation items. Files are removed
 * explicitly by tempfiles_cleanup() or by an atexit handler registered on first creation. Calls
 * must be serialized. */

/* Create and track a mode-0600 temporary file in a private mode-0700 directory under $TMPDIR.
 * Unset, empty, or invalid-UTF-8 TMPDIR values fall back to "/tmp" because paths must remain valid
 * when encoded for the model. Prefix and suffix must be valid UTF-8 filename fragments without
 * '/'; path_out must be non-NULL. Returns the open O_RDWR fd and stores a malloc'd path in
 * *path_out. On failure, returns -1, sets errno, and stores NULL in *path_out. */
int tempfile_create(const char *prefix, const char *suffix, char **path_out);

/* Stop tracking a non-NULL path without unlinking it. The caller retains ownership of the file.
 * An unknown path is ignored. */
void tempfile_untrack(const char *path);

/* Unlink all tracked files and remove empty temporary directories. Directories containing files
 * retained by callers remain available and are retried by later cleanup calls. */
void tempfiles_cleanup(void);

#endif /* HAX_SYSTEM_TEMPFILES_H */
