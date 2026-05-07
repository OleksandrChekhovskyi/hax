/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_RENDER_H
#define HAX_TOOL_RENDER_H

#include <stddef.h>

#include "ctrl_strip.h"
#include "term_lite.h"
#include "utf8_sanitize.h"
#include "util.h" /* struct buf */

/* Streaming tool-output renderer.
 *
 * Tool output flows through this renderer chunk by chunk: bytes arrive
 * via tool_render_feed (driven by the emit_display callback the agent
 * hands to each tool's run()), get sanitized (ctrl_strip, utf8_sanitize),
 * then fed through term_lite so a stream of \r/\b/CRLF rewriting
 * progress lines (ninja, meson, cargo, ...) is interpreted the way a
 * tty would render it. The renderer emits a live dim-styled preview to
 * the terminal in one of three modes:
 *
 *   - R_HEAD_ONLY: dim block capped at HEAD_ONLY_LINES *non-empty*
 *     committed lines. Lines past the cap are counted but not shown;
 *     finalize emits a "... (N more lines, M more bytes)" footer.
 *
 *   - R_HEAD_TAIL: dim block capped at HT_HEAD_LINES non-empty
 *     committed lines. Past the cap, the most recent HT_TAIL_LINES are
 *     kept in a small ring; finalize emits an elision marker plus the
 *     ring contents.
 *
 *   - R_DIFF: line-buffered, per-line colored, uncapped. Finalize
 *     flushes any trailing partial line.
 *
 * In non-diff modes the spinner doubles as a one-line lookahead: it
 * shows the in-progress line from term_lite (or, when cur is blank,
 * the most recently committed line — the renderer's "pending"). A new
 * \n-terminated line doesn't immediately scroll up into the dim block;
 * it just becomes the new pending and the spinner repaints. Only when
 * the *next* non-blank line arrives is the previous pending promoted
 * to a real row. At finalize, the spinner's row is repurposed in place
 * as the closing "└ <pending>" row — no overprint trick, no held-newline
 * shuffling, the cursor naturally lands where the close belongs.
 *
 * Empty / whitespace-only committed lines are dropped from the preview
 * (and don't count toward the head cap) — tools often inject blank
 * spacing that would otherwise eat the small head/tail budget. The
 * model still sees full line structure; that path runs term_lite
 * separately in bash.c's format_run_output.
 *
 * Mode is chosen by the agent at init time from the tool's capabilities.
 * Diff-capable tools (write/edit) are non-streaming, so the agent has
 * the full return string in hand and can switch to R_DIFF based on the
 * `--- ` prefix before feeding — no runtime peek needed. The renderer
 * therefore assumes mode is fixed for the lifetime of one tool call. */

struct disp;
struct spinner;

enum render_mode {
    R_DIFF,      /* line-buffered, per-line color, uncapped */
    R_HEAD_ONLY, /* head cap with "(N more)" footer        */
    R_HEAD_TAIL, /* head cap with elision + tail ring      */
};

/* Tail ring capacity for R_HEAD_TAIL — exposed so tests and the struct
 * layout stay consistent without pulling the whole .c file in. */
#define TOOL_RENDER_TAIL_LINES 4

struct tool_render {
    struct disp *disp;
    struct spinner *spinner;
    struct ctrl_strip strip;
    /* Stateful UTF-8 sanitization: bytes pass through ctrl_strip first
     * (drops C0/escape sequences), then this validator (replaces
     * malformed UTF-8 with U+FFFD). Stateful so a multi-byte codepoint
     * split across emit_display chunks isn't double-FFFD'd. */
    struct utf8_sanitize utf8;
    /* Stateful terminal-lite: interprets \r/\b/CRLF the way a tty
     * would and exposes committed lines + the in-progress buffer. */
    struct term_lite term;

    /* "Did the tool call emit_display?" — set on every tool_render_emit
     * invocation regardless of byte count, so a call that strips to
     * zero clean bytes still counts as having emitted. The dispatch
     * wiring uses this to decide whether to feed the tool's return
     * value through emit_display for one-shot rendering. */
    int emit_called;

    enum render_mode mode;

    int dim_open;       /* ANSI_DIM has been emitted (must close on finalize) */
    int started;        /* any visible byte has been emitted in this block    */
    int spinner_paused; /* hid spinner on first commit; resume after head fills */
    int status_active;  /* spinner-as-status currently shown beneath the head */

    /* True once any preview row has been emitted. Drives strip-glyph
     * selection: the very first row of a multi-row block uses "┌",
     * subsequent body rows use "│", the close uses "└". A block that
     * ends up with only one total row uses "›" instead of "└" so the
     * single-line case reads as a self-contained chevron rather than a
     * dangling corner. */
    int first_emitted;

    /* The most-recently committed non-blank line that we haven't yet
     * promoted into the dim block. Held back deliberately: the spinner
     * row already shows this content as its label, so emitting it as
     * a "│" row right away would be a redundant double-render. When the
     * NEXT non-blank line arrives, the previous pending gets promoted
     * (emit_pipe_line for the head, push to tail or just bump suppressed
     * past head_full) and the new line takes its place. At finalize the
     * pending is the natural close-row content — the spinner row gets
     * repurposed in place as "└ <pending>", no overprint needed. Also
     * acts as the spinner's fallback label when cur goes blank (e.g.
     * after a \r-overwrite that cleared the in-progress buffer). */
    struct buf pending;

    /* Head budget: counts of *non-empty committed lines* emitted into
     * the dim block, and total bytes (line lengths + \n) for the
     * footer. */
    int lines_emitted;
    size_t bytes_emitted;
    int head_full;

    /* R_HEAD_TAIL: ring of recent committed non-empty lines past the
     * head cap. Each slot owns a malloc'd line with no trailing \n. */
    char *tail_lines[TOOL_RENDER_TAIL_LINES];
    size_t tail_lens[TOOL_RENDER_TAIL_LINES];
    int tail_pos;   /* next write index (modulo TOOL_RENDER_TAIL_LINES) */
    int tail_count; /* total lines stored (≤ TOOL_RENDER_TAIL_LINES)    */

    /* Counts of non-empty committed lines and their bytes suppressed
     * past the head cap. Drive the elision marker / footer wording. */
    int suppressed_lines;
    size_t suppressed_bytes;

    /* R_DIFF: per-line buffer so we can color whole lines. */
    struct buf diff_line;
};

void tool_render_init(struct tool_render *r, struct disp *d, struct spinner *sp,
                      enum render_mode mode);
void tool_render_free(struct tool_render *r);

/* Feed sanitized bytes for live preview. */
void tool_render_feed(struct tool_render *r, const char *bytes, size_t n);

/* Close out the preview block: flush UTF-8 tail and any uncommitted
 * line, emit footer/elision, close dim if open. Idempotent for empty
 * output (no preview emitted). */
void tool_render_finalize(struct tool_render *r);

/* tool_emit_display_fn-shaped callback that feeds bytes into the
 * renderer pointed to by `user`. Sets emit_called for every invocation. */
int tool_render_emit(const char *bytes, size_t n, void *user);

#endif /* HAX_TOOL_RENDER_H */
