/* SPDX-License-Identifier: MIT */
#ifndef HAX_DISP_H
#define HAX_DISP_H

#include <stddef.h>

/* Block-aware terminal output.
 *
 * All conversation output (user prompt, model text, tool calls, usage line)
 * flows through these helpers so visual blocks stay separated by exactly
 * one blank line, regardless of how many trailing or leading newlines the
 * model and tools happen to emit.
 *
 * Trailing newline runs are deferred into `held` instead of being committed
 * to stdout, so a later disp_block_separator can cap them at 2 (one blank
 * line). Without buffering they'd already be on the terminal and we
 * couldn't take them back. Within a block, a non-newline byte commits any
 * held newlines verbatim.
 *
 * disp does not own stdout — callers free to fflush/fputs around it as
 * long as they don't write content bytes directly (escapes are fine via
 * disp_raw). */

struct disp {
    int trail;    /* trailing newlines committed to terminal */
    int held;     /* trailing newlines received but not yet committed */
    int saw_text; /* have we emitted real model text yet this turn? */
    /* 1 when the cursor is at column 0 (post-NL) or right after an
     * ASCII space — i.e. a place where overlay glyphs (notably the
     * idle inline spinner) can attach without looking glued to the
     * preceding character. Updated by every byte-committing write so
     * the agent can decide whether to ask the spinner for a leading
     * pad cell. Initially 1 (fresh-line state). */
    int at_space_or_bol;
};

/* Drain held newlines to stdout. */
void disp_emit_held(struct disp *d);

/* Write one byte. '\n' is deferred into held; everything else commits
 * any pending held newlines first. */
void disp_putc(struct disp *d, char c);

/* Write n bytes, deferring trailing newline runs (\n and \r) into held
 * so a later block separator can collapse them. \r alone is treated as a
 * column-zero return — only \n counts as a line break for held. */
void disp_write(struct disp *d, const char *s, size_t n);

/* Write zero-width bytes (ANSI escapes). Caller guarantees no NLs in s.
 * Does not flush held — escapes land ahead of any pending NLs in byte
 * order, but since they're zero-width that's visually identical. */
void disp_raw(const char *s);

/* Formatted write — bytes go through disp_write. */
__attribute__((format(printf, 2, 3))) void disp_printf(struct disp *d, const char *fmt, ...);

/* Bring the terminal trail to exactly one blank line (two NLs). Held NLs
 * from the previous block are dropped, so trailing blank lines in tool
 * output or model text don't leak through. */
void disp_block_separator(struct disp *d);

/* Strip leading newlines from the first delta of a turn — some compat
 * backends (Qwen on oMLX, in particular) prefix the stream with stray
 * newlines that would push the response visually away from the prompt.
 * Spaces and tabs are preserved: leading indentation can be legitimate
 * response content (code blocks, diff context, etc.). Pure — no stdout
 * writes — so the caller can peek before deciding to hide the spinner. */
void disp_first_delta_strip(const struct disp *d, const char **s, size_t *n);

#endif /* HAX_DISP_H */
