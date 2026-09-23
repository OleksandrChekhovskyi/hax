/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_FRONTMATTER_H
#define HAX_TEXT_FRONTMATTER_H

#include <stddef.h>

/* Return the value of a top-level key in the `---`-fenced YAML frontmatter at the start of content,
 * folded onto one trimmed line with each run of whitespace or line breaks as a single space. This
 * reads the string forms skill metadata uses, not full YAML: anchors, tags, and collections come
 * back as plain text. The result is caller-owned valid UTF-8, or NULL when the frontmatter is
 * incomplete or the value is absent, empty, or malformed. */
char *frontmatter_scalar_line(const char *content, size_t content_len, const char *key);

#endif /* HAX_TEXT_FRONTMATTER_H */
