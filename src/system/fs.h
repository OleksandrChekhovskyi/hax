/* SPDX-License-Identifier: MIT */
#ifndef HAX_FS_H
#define HAX_FS_H

#include <stddef.h>

/*
 * Atomic write with a generated unified diff. Used by both `write` and
 * `edit` tools so they share the same mode-preservation, mkdir -p, and
 * tmp-then-rename behavior.
 *
 * Writes `content` to `path`. If the file already exists its mode is
 * preserved; otherwise the umask-default mode is used. Parent directories
 * are created as needed. The new content is staged in a sibling tempfile
 * and rename(2)d into place — readers never see a half-written file.
 *
 * Returns a freshly allocated unified diff on success (empty "" when the
 * content is unchanged). On failure returns NULL and sets *errmsg to a
 * freshly allocated explanation; caller frees errmsg either way.
 *
 * When non-NULL, *out_was_new is set to 1 if `path` did not exist before
 * the call (a fresh file was created) and 0 if an existing file was
 * overwritten. Only meaningful on success — on the NULL/errmsg path
 * *out_was_new is reset to 0 regardless of how far the create attempt
 * got. Lets callers (notably `write`) shorten the model-facing result
 * for new files where the diff is just the content again.
 */
char *fs_write_with_diff(const char *path, const char *content, size_t content_len, char **errmsg,
                         int *out_was_new);

/* mkdir -p: create `path` and any missing intermediate components with
 * mode 0755. Existing directories (or symlinks to them) along the way are
 * success; an existing non-directory anywhere fails with ENOTDIR, like
 * `mkdir -p`. NULL/empty input is a no-op success. Returns 0 on success,
 * -1 with errno set otherwise. */
int fs_mkdir_p(const char *path);

/* which(1): resolve `name` against $PATH, returning the first candidate
 * that is an executable regular file (symlinks followed) as a malloc'd
 * string, or NULL when nothing matches. A `name` containing '/' skips
 * the search and is returned verbatim if it passes the same check.
 * Deliberately stricter than POSIX PATH semantics: empty and relative
 * PATH entries are skipped — resolving an executable out of whatever
 * directory the agent happens to be in is a code-execution footgun. */
char *fs_which(const char *name);

#endif /* HAX_FS_H */
