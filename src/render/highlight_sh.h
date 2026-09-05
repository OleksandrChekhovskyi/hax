/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_HIGHLIGHT_SH_H
#define HAX_RENDER_HIGHLIGHT_SH_H

#include <stddef.h>

/* Minimal line-local shell span classifier for display highlighting. It knows comments,
 * single/double quotes (with backslash escapes outside single quotes), and the common
 * operator characters; everything else — flags, expansions, backticks, heredocs — stays
 * plain. Malformed input fails safe: an unclosed quote colors to end of line. */
enum sh_span_kind {
    SH_SPAN_PLAIN,
    SH_SPAN_STRING,
    SH_SPAN_COMMENT,
    SH_SPAN_OPERATOR,
};

typedef void (*sh_span_fn)(const char *bytes, size_t n, enum sh_span_kind kind, void *user);

/* Classify s into coalesced maximal-kind runs. '\n' is a plain separator that preserves
 * quote state, so a reflowed logical line highlights as one unit; fence callers feed line
 * by line instead. High bytes pass through as plain. */
void highlight_sh(const char *s, size_t n, sh_span_fn emit, void *user);

/* True when a fence info string names a shell language: the first whitespace-delimited
 * token, case-insensitive, is sh, bash, or shell. */
int highlight_sh_lang(const char *info, size_t len);

#endif /* HAX_RENDER_HIGHLIGHT_SH_H */
