/* SPDX-License-Identifier: MIT */
#include "tool_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ansi.h"
#include "disp.h"
#include "spinner.h"
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
}

void tool_render_free(struct tool_render *r)
{
    free(r->tail);
    buf_free(&r->diff_line);
}

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

static void emit_diff_line(struct tool_render *r, const char *line, size_t len, int with_newline)
{
    const char *color = diff_line_color(line, len);
    if (color)
        disp_raw(color);
    disp_write(r->disp, line, len);
    if (color)
        disp_raw(ANSI_RESET);
    if (with_newline)
        disp_putc(r->disp, '\n');
}

/* First-byte hooks: open the ANSI_DIM wrapper (head-only / head-tail
 * modes) and pause the spinner so live text appears cleanly. Diff mode
 * has no dim wrapper — coloring is per-line. */
static void render_first_byte(struct tool_render *r)
{
    if (r->started)
        return;
    r->started = 1;
    if (r->spinner && !r->spinner_paused) {
        spinner_hide(r->spinner);
        r->spinner_paused = 1;
    }
    if (r->mode == R_HEAD_ONLY || r->mode == R_HEAD_TAIL) {
        disp_raw(ANSI_DIM);
        r->dim_open = 1;
    }
}

/* End the live cap line: emit a deferred newline (only if we're not
 * already at column 0), drain held NLs, close the dim block, and resume
 * the spinner. Used at the moment R_HEAD_ONLY hits its cap and at the
 * moment R_HEAD_TAIL's tail ring wraps (i.e., elision is now guaranteed
 * and the screen has stopped scrolling). */
static void close_head_block(struct tool_render *r)
{
    if (r->disp->held == 0 && r->disp->trail == 0)
        disp_putc(r->disp, '\n');
    disp_emit_held(r->disp);
    if (r->dim_open) {
        disp_raw(ANSI_RESET);
        r->dim_open = 0;
    }
    fflush(stdout);
    if (r->spinner) {
        spinner_show(r->spinner);
        r->spinner_paused = 0;
    }
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

/* Live emit one already-clean byte under the active mode. */
static void emit_byte_capped(struct tool_render *r, char ch)
{
    if (r->head_full) {
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
        /* Close the live block at the first byte that makes elision
         * certain. Until this point we keep the dim section open so
         * that an output which ends up fitting under the tail caps can
         * flow inline at finalize with no synthetic break. */
        if (r->mode == R_HEAD_TAIL && !was_eligible && elision_guaranteed(r))
            close_head_block(r);
        return;
    }

    render_first_byte(r);
    disp_write(r->disp, &ch, 1);
    r->bytes_emitted++;
    if (ch == '\n')
        r->lines_emitted++;

    if (r->lines_emitted >= head_lines_cap(r) || r->bytes_emitted >= head_bytes_cap(r)) {
        r->head_full = 1;
        /* R_HEAD_ONLY always ends with a "(N more)" footer — close the
         * live block now so the footer renders cleanly. R_HEAD_TAIL
         * closes too if the cap landed on a newline boundary (line
         * cap, or byte cap that happened to coincide with \n) so the
         * spinner reappears immediately for slow line-based output;
         * mid-line cap defers to avoid a phantom break (see the
         * suppression branch above for the deferred-close path). */
        if (r->mode != R_HEAD_TAIL || ch == '\n')
            close_head_block(r);
    }
}

/* Diff mode: line-buffer until \n, then emit colored. Partial trailing
 * line is held until finalize. */
static void emit_byte_diff(struct tool_render *r, char ch)
{
    render_first_byte(r);
    if (ch == '\n') {
        emit_diff_line(r, r->diff_line.data ? r->diff_line.data : "", r->diff_line.len, 1);
        buf_reset(&r->diff_line);
    } else {
        buf_append(&r->diff_line, &ch, 1);
    }
}

/* Dispatch one already-sanitized byte to the active mode. */
static void emit_clean(struct tool_render *r, char ch)
{
    if (r->mode == R_DIFF)
        emit_byte_diff(r, ch);
    else
        emit_byte_capped(r, ch);
}

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

    for (size_t i = 0; i < on; i++)
        emit_clean(r, out[i]);

    if (out != stack_utf8)
        free(out);
    if (clean != stack_strip)
        free(clean);
    fflush(stdout);
}

static void render_finalize_capped(struct tool_render *r)
{
    if (!r->head_full) {
        /* Output fit entirely under the cap. Close the dim block with a
         * trailing newline if we don't already have one. */
        if (r->disp->held == 0 && r->disp->trail == 0)
            disp_putc(r->disp, '\n');
        return;
    }

    if (r->mode == R_HEAD_TAIL && !elision_guaranteed(r)) {
        /* Cap was reached but the suppressed range was small enough
         * that no elision marker is warranted — the tail back-walk
         * would overlap the head, so just flush whatever's in the ring
         * inline. Re-open dim if close_head_block already ran
         * (line-aligned cap path); for mid-line cap dim is still open
         * and the open is a no-op. */
        if (r->tail_pos > 0) {
            if (!r->dim_open) {
                disp_raw(ANSI_DIM);
                r->dim_open = 1;
            }
            disp_write(r->disp, r->tail, r->tail_pos);
        }
        if (r->disp->held == 0 && r->disp->trail == 0)
            disp_putc(r->disp, '\n');
        return;
    }

    /* Edge case: output landed exactly on the cap with nothing left
     * over (R_HEAD_ONLY: lines_emitted == cap, no further bytes arrived).
     * For R_HEAD_TAIL this branch can't run since we'd have taken the
     * !tail_wrapped path above. Don't re-open ANSI_DIM only to close it
     * again, and don't emit "... (0 more lines, 0 more bytes)". */
    if (r->suppressed_bytes == 0)
        return;

    /* Elision: the cap line was already finalized by close_head_block
     * (R_HEAD_ONLY at cap-hit, R_HEAD_TAIL at tail-wrap), so no extra
     * "\n" needed here before the marker. Re-open dim. */
    if (!r->dim_open) {
        disp_raw(ANSI_DIM);
        r->dim_open = 1;
    }
    if (r->mode == R_HEAD_TAIL) {
        /* Linearize the ring into a contiguous buffer so the tail-trim
         * back-walk is straightforward. Bounded by DISP_HT_TAIL_BYTES so
         * the stack copy is cheap. Elision can fire either via byte
         * overflow (tail_wrapped) or via line count overflow (more than
         * TAIL_LINES newlines suppressed but < TAIL_BYTES) — in the
         * latter case the ring isn't full, so use tail_pos directly. */
        char linear[DISP_HT_TAIL_BYTES];
        size_t linear_len = r->tail_wrapped ? DISP_HT_TAIL_BYTES : r->tail_pos;
        size_t oldest = r->tail_wrapped ? r->tail_pos : 0;
        for (size_t k = 0; k < linear_len; k++)
            linear[k] = r->tail[(oldest + k) % DISP_HT_TAIL_BYTES];

        /* Back-walk to keep at most DISP_HT_TAIL_LINES lines: ignore a
         * trailing \n (so it doesn't count as a boundary), cross
         * TAIL_LINES-1 newlines, then stop at the next newline so the
         * tail begins on a clean line boundary. The byte cap is implicit
         * since the ring is already bounded at TAIL_BYTES. */
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
        /* tail_start now points at the first byte of the kept tail; the
         * kept range runs through the very end of the linearized ring,
         * so the trailing newline (if any) is included naturally. */
        size_t kept = linear_len - tail_start;
        if (kept > r->suppressed_bytes)
            kept = r->suppressed_bytes; /* shouldn't happen */
        size_t mid_bytes = r->suppressed_bytes - kept;
        int mid_lines = r->suppressed_lines;
        /* Newlines inside the kept tail aren't middle bytes. */
        for (size_t k = tail_start; k < linear_len; k++)
            if (linear[k] == '\n')
                mid_lines--;
        if (mid_lines < 0)
            mid_lines = 0;

        /* Defensive: mid_bytes can still be 0 in the rare case where
         * the tail ring wrapped exactly once on a no-newline output
         * (so back-walk keeps the entire ring as tail and nothing was
         * actually dropped). Skip the marker in that case rather than
         * emit "... (0 more lines, 0 more bytes) ...". */
        if (mid_bytes > 0) {
            disp_printf(r->disp, "... (%d more line%s, %zu more byte%s) ...", mid_lines,
                        mid_lines == 1 ? "" : "s", mid_bytes, mid_bytes == 1 ? "" : "s");
            disp_putc(r->disp, '\n');
        }
        if (kept > 0)
            disp_write(r->disp, linear + tail_start, kept);
        if (r->disp->held == 0 && r->disp->trail == 0)
            disp_putc(r->disp, '\n');
    } else { /* R_HEAD_ONLY */
        int more_lines = r->suppressed_lines + r->suppressed_partial_trailing;
        disp_printf(r->disp, "... (%d more line%s, %zu more byte%s)", more_lines,
                    more_lines == 1 ? "" : "s", r->suppressed_bytes,
                    r->suppressed_bytes == 1 ? "" : "s");
        disp_putc(r->disp, '\n');
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
    /* Flush any in-progress UTF-8 sequence as U+FFFD. Drives emit_clean
     * directly so the byte goes through the same head/tail/diff path
     * as live bytes — matters for the "started" flag in particular,
     * which gates the no-output branch below. */
    char tail[UTF8_SANITIZE_FLUSH_MAX];
    size_t tn = utf8_sanitize_flush(&r->utf8, tail);
    for (size_t i = 0; i < tn; i++)
        emit_clean(r, tail[i]);

    if (r->spinner && !r->spinner_paused) {
        spinner_hide(r->spinner);
        r->spinner_paused = 1;
    }

    if (!r->started) {
        /* No output at all. Tools-level convention is "(no output)" for
         * empty bash, but bash now emits that footer itself, so any
         * truly empty stream just leaves no preview block. */
        return;
    }

    if (r->mode == R_DIFF) {
        if (r->diff_line.len > 0) {
            emit_diff_line(r, r->diff_line.data, r->diff_line.len, 1);
            buf_reset(&r->diff_line);
        }
    } else {
        render_finalize_capped(r);
        if (r->dim_open) {
            disp_raw(ANSI_RESET);
            r->dim_open = 0;
        }
    }
    fflush(stdout);
}
