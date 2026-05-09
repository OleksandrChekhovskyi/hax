/* SPDX-License-Identifier: MIT */
#include "tool_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ansi.h"
#include "disp.h"
#include "spinner.h"
#include "utf8.h"
#include "util.h"

/* Head-only preview (file-content tools — read top-down). */
#define DISP_HEAD_ONLY_LINES 8
#define DISP_HEAD_ONLY_BYTES 3000

/* Head + tail preview (command-output tools — errors land at the
 * bottom). Whichever side hits its line or byte cap first stops. */
#define DISP_HT_HEAD_LINES 4
#define DISP_HT_TAIL_LINES 4
#define DISP_HT_HEAD_BYTES 1500
#define DISP_HT_TAIL_BYTES 1500

static int head_lines_cap(const struct tool_render *r)
{
    return r->mode == R_HEAD_TAIL ? DISP_HT_HEAD_LINES : DISP_HEAD_ONLY_LINES;
}

static size_t head_bytes_cap(const struct tool_render *r)
{
    return r->mode == R_HEAD_TAIL ? DISP_HT_HEAD_BYTES : DISP_HEAD_ONLY_BYTES;
}

/* Cell budget for one preview row's content. Subtracts the gutter
 * strip width (DISP_TOOL_STRIP_COLS) plus a one-cell right margin —
 * the margin keeps the cursor strictly under terminal width even when
 * a row fills the budget exactly (content + "..." marker). Some
 * terminals auto-wrap when the cursor lands at col == width (the
 * "phantom space" rule), which would push the held \n onto a wrapped
 * row and break the close-glyph overprint cursor invariant.
 * display_width() caps at DISPLAY_WIDTH_CAP so this stays sane on
 * ultra-wide terminals. */
static size_t content_budget(void)
{
    int w = display_width();
    if (w <= DISP_TOOL_STRIP_COLS + 5)
        return 1;
    return (size_t)(w - DISP_TOOL_STRIP_COLS - 1);
}

void tool_render_init(struct tool_render *r, struct disp *d, struct spinner *sp,
                      enum render_mode mode)
{
    memset(r, 0, sizeof(*r));
    r->disp = d;
    r->spinner = sp;
    ctrl_strip_init(&r->strip);
    utf8_sanitize_init(&r->utf8);
    r->mode = mode;
    if (r->mode == R_HEAD_TAIL)
        r->tail = xmalloc(DISP_HT_TAIL_BYTES);
    buf_init(&r->diff_line);
    buf_init(&r->row_ws);
}

void tool_render_free(struct tool_render *r)
{
    free(r->tail);
    buf_free(&r->diff_line);
    buf_free(&r->row_ws);
}

/* Emit the gutter strip for the row we're about to write content on:
 * "┌" for the very first row of the block, "│" for any subsequent
 * row. Re-opens ANSI_DIM after the strip's ANSI_RESET so the row's
 * content keeps the dim styling (R_DIFF skips this — its content
 * gets per-line color from emit_diff_line). Idempotent within a row
 * via row_strip_emitted. Hides the spinner on first call so live
 * preview rows don't fight the spinner thread. */
static void row_strip_open(struct tool_render *r)
{
    if (r->row_strip_emitted)
        return;
    if (r->spinner && !r->spinner_paused) {
        spinner_hide(r->spinner);
        r->spinner_paused = 1;
    }
    if (r->rows_emitted == 0)
        disp_tool_strip_first(r->disp);
    else
        disp_tool_strip(r->disp);
    if (r->mode != R_DIFF) {
        disp_raw(ANSI_DIM);
        r->dim_open = 1;
    }
    r->row_strip_emitted = 1;
    r->started = 1;
}

/* Close the open row: emit \n (held), update head budget, reset
 * per-row state, and increment rows_emitted so finalize knows whether
 * to overprint with "└" (multi-row) or "›" (single-row). Used by the
 * non-diff path; diff mode does its own row-break inline since the
 * strip+content+\n is one synchronous block.
 *
 * Empty *and* whitespace-only rows (no non-ws codepoint emitted, i.e.
 * !row_strip_emitted) are silently skipped from the preview — no
 * strip, no \n, no counter bumps. Any leading whitespace deferred via
 * row_ws is discarded here. The model still sees the raw bytes via
 * the tool's return string, so the elision is purely a display
 * concern. R_DIFF preserves blank lines (handled in emit_diff_line,
 * which always emits a row). */
static void row_break_capped(struct tool_render *r)
{
    if (!r->row_strip_emitted) {
        buf_reset(&r->row_ws);
        r->row_ws_cells = 0;
        r->row_truncated = 0;
        return;
    }
    disp_putc(r->disp, '\n');
    r->bytes_emitted++;
    r->lines_emitted++;
    r->row_strip_emitted = 0;
    r->row_cells = 0;
    r->row_truncated = 0;
    r->rows_emitted++;
}

/* Flush any deferred leading whitespace into the current row, crediting
 * its bytes against bytes_emitted and its cells against row_cells.
 * Caller guarantees the row strip has been opened (so the bytes land
 * in the right place visually). */
static void row_flush_pending_ws(struct tool_render *r)
{
    if (r->row_ws.len == 0)
        return;
    disp_write(r->disp, r->row_ws.data, r->row_ws.len);
    r->bytes_emitted += r->row_ws.len;
    r->row_cells += r->row_ws_cells;
    buf_reset(&r->row_ws);
    r->row_ws_cells = 0;
}

/* Write one already-clean codepoint into the current row, applying
 * the per-row cell cap. Once row_truncated is set the caller suppresses
 * further bytes until \n. The cap reserves 3 cells eagerly for a
 * potential "..." marker — content that would push row_cells past
 * (budget - 3) trips truncation and the marker is emitted in those
 * reserved cells. Slightly conservative for content that exactly
 * fills the row (loses ≤3 cells when no marker actually was needed),
 * but the alternative — back-stepping with \b — is finicky and the
 * preview is approximate anyway.
 *
 * Leading whitespace (space, tab) that lands before the first non-ws
 * codepoint is buffered into row_ws instead of being emitted live, so
 * a row that turns out to be whitespace-only can be elided at row
 * break. On the first non-ws codepoint the buffer is flushed and the
 * row's strip opens — indentation is preserved for visible rows. */
static void row_emit_codepoint(struct tool_render *r, const char *bytes, size_t len, int cells)
{
    if (r->row_truncated)
        return;
    if (cells < 0)
        cells = 1;
    size_t budget = content_budget();
    size_t safe = budget >= 3 ? budget - 3 : 0;
    int is_ws = (len == 1 && (bytes[0] == ' ' || bytes[0] == '\t'));
    if (is_ws && !r->row_strip_emitted) {
        /* Cap the deferred-whitespace buffer at the row's "safe"
         * budget (cells available before the truncation marker kicks
         * in). Whitespace past that point couldn't render on the row
         * anyway, and the bound keeps a tool that floods spaces
         * without a newline from growing row_ws without limit. With
         * the bound in place the "ws + marker fits" check below
         * always succeeds when the strip is still closed, so
         * over-indented rows render as "<ws> ..." rather than
         * vanishing. */
        if ((size_t)r->row_ws_cells + (size_t)cells <= safe) {
            buf_append(&r->row_ws, bytes, len);
            r->row_ws_cells += cells;
        }
        return;
    }
    /* When strip is closed, row_cells is 0 and ws_cells holds the
     * deferred leading whitespace; when strip is open ws_cells is 0
     * (already flushed). Summing both is safe in either state. */
    if ((size_t)r->row_cells + (size_t)r->row_ws_cells + (size_t)cells > safe) {
        if (budget >= 3 && (size_t)r->row_cells + (size_t)r->row_ws_cells + 3 <= budget) {
            row_strip_open(r);
            row_flush_pending_ws(r);
            disp_write(r->disp, "...", 3);
            r->row_cells += 3;
            /* Don't credit the synthetic "..." against bytes_emitted —
             * the head-bytes cap measures input bytes shown, and
             * counting decoration bytes would make the cap trip
             * earlier than the documented constant suggests. */
        }
        r->row_truncated = 1;
        buf_reset(&r->row_ws);
        r->row_ws_cells = 0;
        return;
    }
    row_strip_open(r);
    row_flush_pending_ws(r);
    disp_write(r->disp, bytes, len);
    r->row_cells += cells;
    r->bytes_emitted += len;
}

/* True once we know finalize will need to emit an elision marker —
 * either the tail ring overflowed or the suppressed range has more
 * newlines than the tail line cap. Below the threshold, the tail
 * back-walk in finalize would not reach back into the head, so the
 * marker would be redundant. */
static int elision_guaranteed(const struct tool_render *r)
{
    return r->tail_wrapped || r->suppressed_lines > DISP_HT_TAIL_LINES;
}

/* End the live cap line: ensure a trailing \n is queued, close the
 * dim block, and mark the held-flush + spinner-show as pending. Used
 * at the moment R_HEAD_ONLY hits its cap and at the moment R_HEAD_TAIL's
 * tail ring wraps (i.e., elision is now guaranteed and the screen has
 * stopped scrolling).
 *
 * If the cap landed mid-line (row still open in our state), bump
 * rows_emitted to count it and reset the per-row state so subsequent
 * emits — the elision marker, tail rows — start clean. Otherwise
 * row_truncated would still be 1 and row_emit_codepoint would silently
 * drop everything that follows.
 *
 * The held \n is deliberately NOT flushed here, nor is the spinner
 * shown — both are deferred to head_close_emit_pending, called on the
 * first suppress_byte. That keeps the cap row's \n in `held` for the
 * common at-cap-no-suppression case (output ends exactly at the cap),
 * so the close-glyph overprint at finalize lands on the cap row's
 * strip rather than a blank row past it. When suppression actually
 * happens, the deferred work fires immediately and the spinner is
 * visible during the long-output gap. */
static void close_head_block(struct tool_render *r)
{
    if (r->disp->held == 0 && r->disp->trail == 0)
        disp_putc(r->disp, '\n');
    if (r->row_strip_emitted) {
        r->rows_emitted++;
        r->row_strip_emitted = 0;
        r->row_cells = 0;
        r->row_truncated = 0;
    }
    if (r->dim_open) {
        disp_raw(ANSI_RESET);
        r->dim_open = 0;
    }
    fflush(stdout);
    r->head_close_pending = 1;
}

/* Flush the deferred close_head_block work: commit the held \n so the
 * cursor moves past the cap row, then show the spinner so the user
 * has visible feedback during the suppression phase. Idempotent;
 * subsequent calls are no-ops. */
static void head_close_emit_pending(struct tool_render *r)
{
    if (!r->head_close_pending)
        return;
    r->head_close_pending = 0;
    disp_emit_held(r->disp);
    fflush(stdout);
    if (r->spinner) {
        spinner_show(r->spinner);
        r->spinner_paused = 0;
    }
}

/* Suppression path: bytes past the head cap are not displayed but are
 * still counted for the elision marker / footer wording, and (for
 * R_HEAD_TAIL) recorded in a ring so finalize can replay the most
 * recent slice. The cap-row close is deferred until elision becomes
 * inevitable so that an output which still fits under the tail caps
 * can flow inline at finalize without a synthetic break. */
static void suppress_byte(struct tool_render *r, char ch)
{
    int was_eligible = elision_guaranteed(r);
    if (r->mode == R_HEAD_TAIL) {
        r->tail[r->tail_pos++] = ch;
        if (r->tail_pos == DISP_HT_TAIL_BYTES) {
            r->tail_pos = 0;
            r->tail_wrapped = 1;
        }
    }
    r->suppressed_bytes++;
    if (ch == '\n') {
        r->suppressed_lines++;
        r->suppressed_partial_trailing = 0;
    } else {
        r->suppressed_partial_trailing = 1;
    }
    if (r->mode == R_HEAD_TAIL && !was_eligible && elision_guaranteed(r))
        close_head_block(r);
    /* If close_head_block ran (just now or earlier), this is the
     * point where its deferred flush + spinner-show kick in: a real
     * byte arrived past the cap, so we know there's more output to
     * cover and the spinner has a job. */
    if (r->head_close_pending)
        head_close_emit_pending(r);
}

/* Stream one already-sanitized codepoint into the head/tail block.
 * Suppression past the cap routes byte-by-byte (the tail ring keeps
 * raw bytes, replayed through the same row machinery at finalize).
 * Within the head, content goes through row_emit_codepoint and \n is
 * a row break; cap-trigger fires once lines_emitted or bytes_emitted
 * meets its threshold. */
static void emit_codepoint_capped(struct tool_render *r, const char *bytes, size_t len, int cells)
{
    if (r->head_full) {
        for (size_t k = 0; k < len; k++)
            suppress_byte(r, bytes[k]);
        return;
    }
    int is_newline = (len == 1 && bytes[0] == '\n');
    if (is_newline) {
        row_break_capped(r);
    } else {
        row_emit_codepoint(r, bytes, len, cells);
    }
    if (r->lines_emitted >= head_lines_cap(r) || r->bytes_emitted >= head_bytes_cap(r)) {
        r->head_full = 1;
        /* R_HEAD_ONLY always ends with a "(N more)" footer — close the
         * live block now so the footer renders cleanly. R_HEAD_TAIL
         * closes too if the cap landed on a newline boundary so the
         * spinner reappears immediately for slow line-based output;
         * mid-line cap defers to avoid a phantom break. */
        if (r->mode != R_HEAD_TAIL || is_newline)
            close_head_block(r);
    }
}

/* ---- Diff mode ---------------------------------------------------- */

static const char *diff_line_color(const char *line, size_t len)
{
    if (len >= 4 && (memcmp(line, "--- ", 4) == 0 || memcmp(line, "+++ ", 4) == 0))
        return ANSI_DIM; /* file headers — scaffolding, not signal */
    if (len >= 2 && memcmp(line, "@@", 2) == 0)
        return ANSI_DIM;
    if (len >= 1 && line[0] == '+')
        return ANSI_GREEN;
    if (len >= 1 && line[0] == '-')
        return ANSI_RED;
    if (len >= 1 && line[0] == '\\')
        return ANSI_DIM; /* \ No newline at end of file */
    return NULL;
}

/* Emit one diff line: gutter strip, color, truncated content, \n.
 * Truncation goes through truncate_for_display so it lands on a
 * codepoint boundary and appends "..." when the row would overflow.
 * The leading-color SGR is closed with ANSI_RESET before the \n so
 * the held newline carries no residual styling. */
static void emit_diff_line(struct tool_render *r, const char *line, size_t len)
{
    if (r->spinner && !r->spinner_paused) {
        spinner_hide(r->spinner);
        r->spinner_paused = 1;
    }
    if (r->rows_emitted == 0)
        disp_tool_strip_first(r->disp);
    else
        disp_tool_strip(r->disp);
    r->row_strip_emitted = 1;
    r->started = 1;
    const char *color = diff_line_color(line, len);
    if (color)
        disp_raw(color);
    /* truncate_for_display takes a NUL-terminated string; diff_line is
     * a struct buf which is NUL-terminated at .len. Empty buf has NULL
     * data — feed an empty literal. */
    char *trimmed = truncate_for_display(line ? line : "", content_budget());
    disp_write(r->disp, trimmed, strlen(trimmed));
    free(trimmed);
    if (color)
        disp_raw(ANSI_RESET);
    disp_putc(r->disp, '\n');
    r->rows_emitted++;
    r->row_strip_emitted = 0;
}

static void emit_byte_diff(struct tool_render *r, char ch)
{
    if (ch == '\n') {
        emit_diff_line(r, r->diff_line.data ? r->diff_line.data : "", r->diff_line.len);
        buf_reset(&r->diff_line);
    } else {
        buf_append(&r->diff_line, &ch, 1);
    }
}

/* ---- Public feed/finalize ---------------------------------------- */

void tool_render_feed(struct tool_render *r, const char *bytes, size_t n)
{
    if (n == 0)
        return;
    /* Two-stage sanitize: ctrl_strip drops C0/escape sequences (never
     * expands, so n bytes in → ≤ n bytes out). utf8_sanitize then
     * replaces malformed bytes with U+FFFD (worst case 3x expansion),
     * holding partial multi-byte sequences across chunks so a codepoint
     * split at an emit_display-chunk boundary isn't double-replaced. */
    char stack_strip[4096];
    char *clean = n <= sizeof(stack_strip) ? stack_strip : xmalloc(n);
    size_t cn = ctrl_strip_feed(&r->strip, bytes, n, clean);

    char stack_utf8[UTF8_SANITIZE_OUT_MAX(4096)];
    size_t need = UTF8_SANITIZE_OUT_MAX(cn);
    char *out = need <= sizeof(stack_utf8) ? stack_utf8 : xmalloc(need);
    size_t on = utf8_sanitize_feed(&r->utf8, clean, cn, out);

    if (r->mode == R_DIFF) {
        for (size_t i = 0; i < on; i++)
            emit_byte_diff(r, out[i]);
    } else {
        size_t i = 0;
        while (i < on) {
            char c = out[i];
            if (c == '\n') {
                emit_codepoint_capped(r, &c, 1, 0);
                i++;
                continue;
            }
            if (c == '\t') {
                /* Pass tab through unchanged — terminals expand it to
                 * the next tab stop. Counting it as 1 cell slightly
                 * underestimates width; preview is approximate anyway. */
                emit_codepoint_capped(r, "\t", 1, 1);
                i++;
                continue;
            }
            size_t consumed = 0;
            int cells = utf8_codepoint_cells(out, on, i, &consumed);
            if (consumed == 0) {
                /* Defensive — utf8_sanitize should leave clean UTF-8. */
                consumed = 1;
            }
            if (cells < 0) {
                /* Unprintable / dangerous codepoint (Trojan Source bidi,
                 * stray controls). Substitute one '?' to mirror the
                 * flatten_for_display policy — keeps the visual row
                 * width predictable. */
                emit_codepoint_capped(r, "?", 1, 1);
            } else {
                emit_codepoint_capped(r, out + i, consumed, cells);
            }
            i += consumed;
        }
    }

    if (out != stack_utf8)
        free(out);
    if (clean != stack_strip)
        free(clean);
    fflush(stdout);
}

/* Walk a tail buffer (already-sanitized bytes from the ring) through
 * the row machinery, so each line gets its own gutter strip and the
 * per-row truncation cap. Unlike the live path, head_full is true
 * here — we route content through row_emit_codepoint / row_break_capped
 * directly, bypassing the suppress / cap-trigger logic. The empty /
 * whitespace-only row skip lives in row_break_capped, so the tail
 * inherits the same elision policy as the head. */
static void emit_tail_bytes(struct tool_render *r, const char *bytes, size_t n)
{
    size_t i = 0;
    while (i < n) {
        char c = bytes[i];
        if (c == '\n') {
            row_break_capped(r);
            i++;
            continue;
        }
        if (c == '\t') {
            row_emit_codepoint(r, "\t", 1, 1);
            i++;
            continue;
        }
        size_t consumed = 0;
        int cells = utf8_codepoint_cells(bytes, n, i, &consumed);
        if (consumed == 0)
            consumed = 1;
        if (cells < 0)
            row_emit_codepoint(r, "?", 1, 1);
        else
            row_emit_codepoint(r, bytes + i, consumed, cells);
        i += consumed;
    }
}

/* Emit a one-row dim row for an elision / footer marker. Goes through
 * truncate_for_display to fit the budget directly, then writes via
 * disp_write — bypassing row_emit_codepoint's per-row cap (which
 * reserves 3 cells for its own "..." marker and would otherwise
 * re-truncate the marker into gibberish on narrow terminals). */
static void emit_marker_row(struct tool_render *r, const char *text)
{
    if (!r->dim_open) {
        disp_raw(ANSI_DIM);
        r->dim_open = 1;
    }
    row_strip_open(r);
    char *trimmed = truncate_for_display(text, content_budget());
    disp_write(r->disp, trimmed, strlen(trimmed));
    free(trimmed);
    disp_putc(r->disp, '\n');
    r->row_strip_emitted = 0;
    r->row_cells = 0;
    r->row_truncated = 0;
    r->rows_emitted++;
}

static void render_finalize_capped(struct tool_render *r)
{
    if (!r->head_full)
        return;

    if (r->mode == R_HEAD_TAIL && !elision_guaranteed(r)) {
        /* Tail fits inline — no marker, just replay the ring as
         * additional rows. Re-open dim if close_head_block already
         * ran; usually it hasn't (mid-line cap defers the close). */
        if (r->tail_pos > 0) {
            int reopened_dim = !r->dim_open;
            if (reopened_dim) {
                disp_raw(ANSI_DIM);
                r->dim_open = 1;
            }
            int rows_before = r->rows_emitted;
            emit_tail_bytes(r, r->tail, r->tail_pos);
            /* If close_head_block ran (cap landed on a newline) and the
             * suppressed bytes were all blank/ws — emit_tail_bytes
             * skipped every row — head_close_emit_pending has already
             * committed the cap row's held \n, so the cursor sits on a
             * new blank row past the cap. Without a strip on that row
             * the close-glyph overprint at finalize would land on bare
             * terrain. Emit a placeholder strip so └ has a gutter to
             * overprint. reopened_dim is the signal: dim is only
             * closed by close_head_block, and tail_pos > 0 in this
             * branch implies suppress_byte ran ≥ once (which fires
             * head_close_emit_pending if pending). */
            if (reopened_dim && r->rows_emitted == rows_before) {
                row_strip_open(r);
                row_break_capped(r);
            }
        }
        return;
    }

    /* Output landed exactly on the cap with nothing left over — don't
     * re-open dim only to close it again, and don't emit
     * "... (0 more lines, 0 more bytes)". */
    if (r->suppressed_bytes == 0)
        return;

    if (r->mode == R_HEAD_TAIL) {
        /* Linearize the ring into a contiguous buffer so the tail-trim
         * back-walk is straightforward. Bounded by DISP_HT_TAIL_BYTES so
         * the stack copy is cheap. */
        char linear[DISP_HT_TAIL_BYTES];
        size_t linear_len = r->tail_wrapped ? DISP_HT_TAIL_BYTES : r->tail_pos;
        size_t oldest = r->tail_wrapped ? r->tail_pos : 0;
        for (size_t k = 0; k < linear_len; k++)
            linear[k] = r->tail[(oldest + k) % DISP_HT_TAIL_BYTES];

        /* Back-walk to keep at most DISP_HT_TAIL_LINES lines. */
        size_t tail_start = linear_len;
        if (tail_start > 0 && linear[tail_start - 1] == '\n')
            tail_start--;
        int crossed = 0;
        while (tail_start > 0) {
            if (linear[tail_start - 1] == '\n') {
                if (crossed == DISP_HT_TAIL_LINES - 1)
                    break;
                crossed++;
            }
            tail_start--;
        }
        size_t kept = linear_len - tail_start;
        if (kept > r->suppressed_bytes)
            kept = r->suppressed_bytes;
        size_t mid_bytes = r->suppressed_bytes - kept;
        int mid_lines = r->suppressed_lines;
        for (size_t k = tail_start; k < linear_len; k++)
            if (linear[k] == '\n')
                mid_lines--;
        if (mid_lines < 0)
            mid_lines = 0;

        if (mid_bytes > 0) {
            /* Wide enough that gcc's -Wformat-truncation can prove a worst-
             * case "%d/%zu" expansion fits without analyzing value ranges. */
            char marker[96];
            snprintf(marker, sizeof(marker), "... (%d more line%s, %zu more byte%s) ...", mid_lines,
                     mid_lines == 1 ? "" : "s", mid_bytes, mid_bytes == 1 ? "" : "s");
            emit_marker_row(r, marker);
        }
        if (kept > 0)
            emit_tail_bytes(r, linear + tail_start, kept);
    } else { /* R_HEAD_ONLY */
        int more_lines = r->suppressed_lines + r->suppressed_partial_trailing;
        /* Wide enough that gcc's -Wformat-truncation can prove a worst-
         * case "%d/%zu" expansion fits without analyzing value ranges. */
        char marker[96];
        snprintf(marker, sizeof(marker), "... (%d more line%s, %zu more byte%s)", more_lines,
                 more_lines == 1 ? "" : "s", r->suppressed_bytes,
                 r->suppressed_bytes == 1 ? "" : "s");
        emit_marker_row(r, marker);
    }
}

int tool_render_emit(const char *bytes, size_t n, void *user)
{
    struct tool_render *r = user;
    r->emit_called = 1;
    tool_render_feed(r, bytes, n);
    return 0;
}

void tool_render_finalize(struct tool_render *r)
{
    /* Flush any in-progress UTF-8 sequence as U+FFFD. */
    char tail[UTF8_SANITIZE_FLUSH_MAX];
    size_t tn = utf8_sanitize_flush(&r->utf8, tail);
    if (tn > 0)
        tool_render_feed(r, tail, tn);

    /* Drain the diff line buffer (R_DIFF) or close an open mid-line row
     * (other modes) before the !started gate — otherwise input that
     * arrived without a trailing \n leaves nothing to render and the
     * gate would skip the flush entirely. */
    if (r->mode == R_DIFF) {
        if (r->diff_line.len > 0) {
            emit_diff_line(r, r->diff_line.data, r->diff_line.len);
            buf_reset(&r->diff_line);
        }
    } else if (r->row_strip_emitted) {
        row_break_capped(r);
    }

    if (r->spinner && !r->spinner_paused) {
        spinner_hide(r->spinner);
        r->spinner_paused = 1;
    }

    if (!r->started) {
        /* No visible output at all — leave no preview block. */
        return;
    }

    if (r->mode != R_DIFF) {
        render_finalize_capped(r);
        /* If the elision/tail flush left a row open (e.g. tail ran out
         * of bytes mid-line), close it so the held \n exists for the
         * close-glyph overprint to land on. */
        if (r->row_strip_emitted)
            row_break_capped(r);
        if (r->dim_open) {
            disp_raw(ANSI_RESET);
            r->dim_open = 0;
        }
    }

    /* Overprint the most recently emitted strip with the close glyph.
     * "└" for multi-row blocks, "›" for a block that ended up with
     * only one row — promotes the leading "┌" to a self-contained
     * chevron. The cursor is still on the last content row (its
     * trailing \n is held, not committed), so \r lands on col 0 of
     * that row and the glyph clobbers the existing strip cell. */
    if (r->rows_emitted >= 2)
        disp_tool_strip_close();
    else if (r->rows_emitted == 1)
        disp_tool_strip_close_solo();

    fflush(stdout);
}
