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
 * emits a live dim-styled preview to the terminal in one of three modes:
 *
 *   - R_HEAD_ONLY: dim block capped at HEAD_ONLY_LINES / HEAD_ONLY_BYTES.
 *     Past the cap, live emission stops and a spinner resumes; finalize
 *     emits a "... (N more lines, M more bytes)" footer.
 *
 *   - R_HEAD_TAIL: dim block capped at HT_HEAD_*; past the cap, live
 *     emission stops, a tail ring captures the most recent HT_TAIL_BYTES,
 *     and finalize emits an elision marker + the tail.
 *
 *   - R_DIFF: line-buffered, per-line colored, uncapped. Finalize
 *     flushes any trailing partial line.
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

    int dim_open;       /* ANSI_DIM has been emitted (must close on finalize) */
    int started;        /* any visible byte has been emitted in this block    */
    int spinner_paused; /* hid spinner on first byte; resume after head fills */
    /* close_head_block ran but its held-\n flush + spinner show have
     * been deferred. Cleared on first suppress_byte (so the spinner
     * shows during long output). If finalize runs with this still set,
     * the cap row's \n is still held — the close-glyph overprint can
     * land on the cap row's strip rather than a blank row past it. */
    int head_close_pending;

    /* Per-row state for the gutter strip and the per-row cell cap.
     * Cleared at every \n so each new row gets its own "┌"/"│" prefix
     * and a fresh budget. row_truncated is set when the row would
     * overflow the cell budget — further bytes on that row are dropped
     * silently until \n. */
    int row_strip_emitted;
    int row_cells;
    int row_truncated;

    /* Leading-whitespace deferral. Spaces and tabs before the first
     * non-whitespace codepoint of a row are buffered here instead of
     * being emitted live; if the row turns out to be whitespace-only
     * it's elided at row break (same skip path as truly-empty rows).
     * On the first non-ws codepoint, row_ws is flushed verbatim so
     * indentation is preserved for visible rows. */
    struct buf row_ws;
    int row_ws_cells;

    /* Total rows fully emitted across head + finalize phases. Drives
     * the close-glyph decision: 1 → overprint with "›", >=2 → "└". */
    int rows_emitted;

    /* Head-budget tracking (R_HEAD_ONLY and R_HEAD_TAIL). */
    int lines_emitted;
    size_t bytes_emitted;
    int head_full;

    /* R_HEAD_TAIL: tail ring captures bytes after the head cap. */
    char *tail;
    size_t tail_pos;
    int tail_wrapped;

    /* Counts of bytes/lines suppressed past the head cap. */
    size_t suppressed_bytes;
    int suppressed_lines;
    /* 1 when the last suppressed byte was not a newline, i.e. there's
     * a partial trailing line beyond the cap. R_HEAD_ONLY adds this
     * to the footer's line count so a long unterminated remainder
     * doesn't read as "0 more lines". */
    int suppressed_partial_trailing;

    /* R_DIFF: per-line buffer so we can color whole lines. */
    struct buf diff_line;
};

void tool_render_init(struct tool_render *r, struct disp *d, struct spinner *sp,
                      enum render_mode mode);
void tool_render_free(struct tool_render *r);

/* Feed sanitized bytes for live preview. */
void tool_render_feed(struct tool_render *r, const char *bytes, size_t n);

/* Close out the preview block: flush UTF-8 tail, emit footer/elision,
 * close dim if open. Idempotent for empty output (no preview emitted). */
void tool_render_finalize(struct tool_render *r);

/* tool_emit_display_fn-shaped callback that feeds bytes into the
 * renderer pointed to by `user`. Sets emit_called for every invocation. */
int tool_render_emit(const char *bytes, size_t n, void *user);

#endif /* HAX_TOOL_RENDER_H */
