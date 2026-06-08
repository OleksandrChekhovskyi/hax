/* SPDX-License-Identifier: MIT */
#ifndef HAX_HTML_MARKDOWN_H
#define HAX_HTML_MARKDOWN_H

#include <stddef.h>

/*
 * Convert an HTML document to a Markdown-ish plain-text rendition suitable
 * for feeding to a model: headings become `#` prefixes, list items `- `,
 * links `[text](href)`, `<pre>` blocks fenced, and everything else is tag-
 * stripped with whitespace collapsed and HTML entities decoded. This is a
 * deliberately small single-pass scanner, not a conforming parser — it
 * trades fidelity for zero dependencies (see web_fetch's design notes). The
 * content of <script>/<style>/<head>/<svg> and similar non-prose elements
 * is dropped wholesale.
 *
 * Returns a freshly-allocated NUL-terminated buffer (caller frees); never
 * NULL. `*out_len`, when non-NULL, receives the byte length excluding the
 * terminator. `len` is the input byte length (the input need not be NUL-
 * terminated).
 */
char *html_to_markdown(const char *html, size_t len, size_t *out_len);

#endif /* HAX_HTML_MARKDOWN_H */
