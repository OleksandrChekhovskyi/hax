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

/* Visual gutter prefix for the body rows of a tool-output block — a
 * thin "│ " in dim cyan so the block reads as a quiet vertical rule
 * down the side. Self-contained: emits ANSI_DIM_CYAN, glyph + space,
 * then ANSI_RESET so callers can apply their own SGR to the content
 * that follows. Goes through disp_write so any held \n from the
 * previous row is flushed first. */
void disp_tool_strip(struct disp *d);

/* Cell width of any of the strip prefixes — one box-drawing glyph plus
 * one trailing space. Useful for budget calculations when truncating
 * line content to fit one terminal row. */
#define DISP_TOOL_STRIP_COLS 2

/* Top row of a multi-line tool block: "┌ " — top-left corner. Same
 * shape and invariants as disp_tool_strip, just a different glyph.
 * Use only on the first row of a streaming block; the closing glyph
 * is chosen at finalize via disp_tool_strip_close{,_solo}. */
void disp_tool_strip_first(struct disp *d);

/* Single-row tool block (skipped / refused tool, "(no changes)" marker,
 * a one-line tool preview): "› " — a small right-facing chevron.
 * Same shape and invariants as disp_tool_strip; used eagerly by the
 * non-streaming inline markers in agent.c. */
void disp_tool_strip_solo(struct disp *d);

/* Close the open tool block by overprinting the most recently emitted
 * strip with the appropriate end-of-block glyph. Writes \r + the new
 * glyph directly to stdout (bypassing disp's held / trail tracking).
 * Caller invariant: the cursor is on the same row as the strip about
 * to be overprinted, with the row's trailing \n still held in disp —
 * i.e. the row's content was the last thing to write to that row.
 *
 * The two variants exist because the streaming path doesn't know its
 * row count up front: at finalize, pick "└" if more than one row was
 * emitted (multi-row close), or "›" if only one (effectively promotes
 * the leading "┌" to a solo chevron). */
void disp_tool_strip_close(void);
void disp_tool_strip_close_solo(void);

#endif /* HAX_DISP_H */
