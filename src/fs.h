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

#endif /* HAX_FS_H */
