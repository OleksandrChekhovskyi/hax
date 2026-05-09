/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_RENDER_H
#define HAX_TOOL_RENDER_H

#include <stddef.h>

#include "ctrl_strip.h"
#include "utf8_sanitize.h"
#include "util.h" /* struct buf */

/* Streaming tool-output renderer.
 *
 * Tool output flows through this renderer chunk by chunk: bytes arrive
 * via tool_render_feed (driven by the emit_display callback the agent
 * hands to each tool's run()), and the renderer ctrl_strips them and
 * emits a live preview to the terminal in one of three modes:
 *
 *   - R_HEAD_ONLY / R_HEAD_TAIL: line-buffered. Bytes accumulate until
 *     \n, then the completed line is classified. Empty/whitespace-only
 *     lines are elided; non-blank lines drive the status row at the
 *     bottom of the live preview — a single row that shows a clock-
 *     derived spinner glyph as its gutter prefix and the last non-
 *     blank line as its content.
 *
 *     Pre-cap: each new non-blank line commits the previous status as
 *     a permanent row (spinner glyph swaps to "│"/"┌", \n queued) and
 *     repaints the status row on the new bottom row with the new
 *     content. Post-cap: the status row stays in place and is repainted
 *     on each new non-blank line; the bytes also feed the suppression
 *     counters and (R_HEAD_TAIL only) the tail ring.
 *
 *     At finalize the status row is replaced based on suppression state:
 *     "(N more …)" footer for R_HEAD_ONLY past the cap; elision marker
 *     + tail rows for R_HEAD_TAIL with elision; tail rows in place for
 *     R_HEAD_TAIL where the tail fits inline; or simply committed as
 *     the final row when no suppression occurred.
 *
 *   - R_DIFF: line-buffered, per-line colored, uncapped, no status row.
 *     Diffs are non-streaming (the agent has the whole string in hand
 *     before feeding) so the live-preview machinery has no purpose;
 *     each line emits as a normal colored row with strip prefix.
 *
 * Mode is chosen by the agent at init time from the tool's capabilities.
 * Diff-capable tools (write/edit) are non-streaming, so the agent has
 * the full return string in hand and can switch to R_DIFF based on the
 * `--- ` prefix before feeding — no runtime peek needed. */

struct disp;
struct spinner;

enum render_mode {
    R_DIFF,      /* line-buffered, per-line color, uncapped */
    R_HEAD_ONLY, /* head cap with "(N more)" footer        */
    R_HEAD_TAIL, /* head cap with elision + tail ring      */
};

struct tool_render {
    struct disp *disp;
    struct spinner *spinner;
    struct ctrl_strip strip;
    /* Stateful UTF-8 sanitization: bytes pass through ctrl_strip first
     * (drops C0/escape sequences), then this validator (replaces
     * malformed UTF-8 with U+FFFD). Stateful so a multi-byte codepoint
     * split across emit_display chunks isn't double-FFFD'd. */
    struct utf8_sanitize utf8;
    /* "Did the tool call emit_display?" — set on every tool_render_emit
     * invocation regardless of byte count, so a call that strips to
     * zero clean bytes still counts as having emitted. The dispatch
     * wiring uses this to decide whether to feed the tool's return
     * value through emit_display for one-shot rendering. */
    int emit_called;

    enum render_mode mode;

    /* Has any visible row or status paint been emitted in this block? */
    int started;

    /* In-progress line: bytes accumulate here between \n boundaries.
     * The buffer is capped (see LINE_BUF_CAP in tool_render.c) so a
     * tool that emits unbounded bytes without a newline doesn't grow
     * memory without limit. line_total_bytes tracks the true byte
     * count for the suppression counter; line_saw_non_ws is the
     * blank-vs-non-blank classification flag (set on any byte not in
     * " \t" so it stays accurate regardless of the buffer cap).
     * line_display_bytes counts the (possibly substituted) bytes
     * pushed to the tail ring for the current line — used at
     * head_full transition to retroactively credit the cap-tripping
     * line's already-pushed bytes to suppressed_display_bytes. */
    struct buf line;
    size_t line_total_bytes;
    size_t line_display_bytes;
    int line_saw_non_ws;

    /* Status row state (R_HEAD_ONLY / R_HEAD_TAIL). status_painted is
     * 1 when the bottom row of the live preview is currently showing
     * `<spinner glyph> <status_content>`; the cursor is parked at the
     * end of the content ready for the next \r-rewind repaint. */
    struct buf status_content;
    int status_painted;

    /* Head accounting. rows_emitted is the number of permanent rows
     * committed so far (status doesn't count). lines_emitted /
     * bytes_emitted are the visible-content counters that decide when
     * the cap fires; head_full is set once either threshold is hit. */
    int rows_emitted;
    int lines_emitted;
    size_t bytes_emitted;
    int head_full;

    /* R_HEAD_TAIL: tail ring captures suppressed bytes after head_full. */
    char *tail;
    size_t tail_pos;
    int tail_wrapped;

    /* Suppression counters driving the elision marker / footer text.
     * suppressed_lines counts non-blank lines past the head cap
     * (blank lines are silently elided everywhere — head, tail, pre-
     * cap — so they don't appear in user-facing counts either). An
     * unterminated trailing line counts as one line if non-blank.
     * suppressed_display_bytes is internal to build_tail_view's ring
     * slice math: bytes pushed to the tail ring under head_full,
     * which can diverge from input byte count when dangerous
     * codepoints are substituted (3-byte U+202E → 1-byte "?"). */
    int suppressed_lines;
    size_t suppressed_display_bytes;

    /* R_DIFF: per-line buffer so we can color whole lines. */
    struct buf diff_line;
};

void tool_render_init(struct tool_render *r, struct disp *d, struct spinner *sp,
                      enum render_mode mode);
void tool_render_free(struct tool_render *r);

/* Feed sanitized bytes for live preview. */
void tool_render_feed(struct tool_render *r, const char *bytes, size_t n);

/* Close out the preview block: flush UTF-8 tail, replace the status
 * row with footer / elision / committed last line as appropriate,
 * emit close glyph. Idempotent for empty output (no preview emitted). */
void tool_render_finalize(struct tool_render *r);

/* tool_emit_display_fn-shaped callback that feeds bytes into the
 * renderer pointed to by `user`. Sets emit_called for every invocation. */
int tool_render_emit(const char *bytes, size_t n, void *user);

#endif /* HAX_TOOL_RENDER_H */
