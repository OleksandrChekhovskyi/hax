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
 */
char *fs_write_with_diff(const char *path, const char *content, size_t content_len, char **errmsg);

#endif /* HAX_FS_H */
