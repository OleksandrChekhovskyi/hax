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

/* Head + tail preview (command-output tools — errors land at the
 * bottom). The tail capacity is in TOOL_RENDER_TAIL_LINES (header). */
#define DISP_HT_HEAD_LINES 4

/* Cap on per-line bytes shown in the dim block when the host terminal
 * width can't be detected. Aggressively trimmed because the transcript
 * view shows raw output verbatim — preview only needs to convey gist. */
#define PREVIEW_FALLBACK_COLS 200

/* The spinner draws "<glyph> <label>". Reserve two cells for the glyph
 * and the separating space so the truncated label fits on one row. */
#define STATUS_GLYPH_RESERVE 2

static int head_lines_cap(const struct tool_render *r)
{
    return r->mode == R_HEAD_TAIL ? DISP_HT_HEAD_LINES : DISP_HEAD_ONLY_LINES;
}

/* Decode the UTF-8 codepoint starting at `s[0]` and return its byte
 * length. Tolerates malformed input by treating any leading byte we
 * don't recognize as a 1-byte "codepoint" so the caller still makes
 * progress. ctrl_strip + utf8_sanitize feed us valid UTF-8 in normal
 * operation, so the fallback is purely defensive. */
static size_t utf8_step(const char *s, size_t n)
{
    if (n == 0)
        return 0;
    unsigned char c = (unsigned char)s[0];
    size_t step;
    if (c < 0x80)
        step = 1;
    else if ((c & 0xE0) == 0xC0)
        step = 2;
    else if ((c & 0xF0) == 0xE0)
        step = 3;
    else if ((c & 0xF8) == 0xF0)
        step = 4;
    else
        step = 1;
    if (step > n)
        step = n;
    return step;
}

/* Truncate `s` (n bytes, valid UTF-8) so the visible result fits in
 * `cols` cells, approximating one column per codepoint. When trimming
 * occurs, the trailing codepoints are replaced with ASCII "..." (three
 * cells, three bytes) — using ASCII rather than the Unicode ellipsis
 * U+2026 keeps the marker readable in fonts that lack the glyph and
 * matches the convention used elsewhere in the UI (truncate_for_display
 * in agent.c, the "[bytes elided]" marker in bash.c). Returns bytes
 * written to `out`. `out` must have room for n + 3 bytes. cols == 0
 * produces empty output. */
static size_t truncate_to_cols(const char *s, size_t n, size_t cols, char *out)
{
    if (cols == 0)
        return 0;
    /* Count codepoints in one pass. */
    size_t cp = 0;
    for (size_t i = 0; i < n;) {
        i += utf8_step(s + i, n - i);
        cp++;
    }
    if (cp <= cols) {
        memcpy(out, s, n);
        return n;
    }
    /* Truncation needed. Reserve 3 cells for "...", or just emit dots
     * when the budget is too tight to fit any original content. */
    if (cols < 3) {
        for (size_t k = 0; k < cols; k++)
            out[k] = '.';
        return cols;
    }
    size_t budget = cols - 3;
    size_t i = 0, w = 0;
    while (budget > 0 && i < n) {
        size_t step = utf8_step(s + i, n - i);
        memcpy(out + w, s + i, step);
        w += step;
        i += step;
        budget--;
    }
    out[w++] = '.';
    out[w++] = '.';
    out[w++] = '.';
    return w;
}

/* Width budget for one preview row. term_width() is wrapped here so a
 * future caller can override (e.g. tests that pin a width); for now
 * we just delegate. The 1-cell margin keeps hard-wrap from happening
 * on the boundary and matches the way other dim renderers in the
 * codebase leave a column. */
static size_t preview_cols(void)
{
    int w = term_width();
    if (w <= 1)
        return PREVIEW_FALLBACK_COLS;
    return (size_t)(w - 1);
}

void tool_render_init(struct tool_render *r, struct disp *d, struct spinner *sp,
                      enum render_mode mode)
{
    memset(r, 0, sizeof(*r));
    r->disp = d;
    r->spinner = sp;
    ctrl_strip_init(&r->strip);
    utf8_sanitize_init(&r->utf8);
    term_lite_init(&r->term);
    buf_init(&r->pending);
    r->mode = mode;
    buf_init(&r->diff_line);
    /* The agent shows its "running..." spinner before handing the
     * renderer the run() call, so by the time we get here the spinner
     * is already painted on the row immediately below the tool header.
     * Treat that as our active status line — otherwise the very first
     * committed line writes content at the cursor position (end of
     * "running...") and the spinner thread, which still thinks the
     * spinner is visible, fires moments later with \r\033[K and
     * erases the content. */
    if (sp) {
        r->status_active = 1;
        /* Tint the streaming-row spinner glyph dim cyan to match the
         * tool block's gutter strip — the spinner row sits at the
         * bottom of the dim block and reads as a continuation of the
         * gutter while output is still arriving. Reverted to plain
         * dim in tool_render_free so the agent's between-turn
         * "working..." / "thinking..." / "running..." status keeps
         * its plain dim styling. */
        spinner_set_cyan(sp, 1);
    }
}

void tool_render_free(struct tool_render *r)
{
    /* Drop the cyan styling so the next spinner_show (e.g. agent's
     * between-turn "working...") draws in plain dim. The spinner
     * itself outlives the tool_render and is reused. */
    if (r->spinner)
        spinner_set_cyan(r->spinner, 0);
    buf_free(&r->pending);
    term_lite_free(&r->term);
    for (int i = 0; i < TOOL_RENDER_TAIL_LINES; i++)
        free(r->tail_lines[i]);
    buf_free(&r->diff_line);
}

/* True when the buffer contains only space/tab (or is empty). Matches
 * term_lite_cur_is_blank's predicate so we treat committed lines
 * the same way as the in-progress buffer. */
static int bytes_are_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c != ' ' && c != '\t')
            return 0;
    }
    return 1;
}

/* Hide the status spinner if it's currently up. Cursor lands at col 0
 * of the (now-erased) status row — the right place to either start a
 * new committed line (which will scroll up into the dim block) or to
 * write the elision marker at finalize. */
static void status_hide(struct tool_render *r)
{
    if (!r->spinner || !r->status_active)
        return;
    spinner_hide(r->spinner);
    r->status_active = 0;
}

/* Write `text` (raw bytes) into the spinner label after truncating to
 * fit one terminal row. Idempotent; safe to call repeatedly with the
 * same content.
 *
 * Flushes any deferred newlines via disp_emit_held first. The spinner
 * positions itself with \r, which lands at column 0 of the current
 * cursor row — and after promote_pending writes a "│" content row, the
 * cursor's row is that just-written content (the \n is still buffered
 * in disp.held). Without the flush, the spinner overprints it. The
 * flush also brings disp.trail in line with the actual terminal state
 * so block_separator math at the next block stays sound. */
static void status_paint(struct tool_render *r, const char *text, size_t n)
{
    if (!r->spinner)
        return;
    disp_emit_held(r->disp);
    size_t cols = preview_cols();
    size_t budget = cols > STATUS_GLYPH_RESERVE ? cols - STATUS_GLYPH_RESERVE : 1;
    /* +3 for the worst-case "..." insertion. +1 for NUL terminator the
     * spinner copies via xstrdup. */
    char *label = xmalloc(n + 4);
    size_t w = truncate_to_cols(text, n, budget, label);
    label[w] = '\0';
    spinner_set_label(r->spinner, label);
    free(label);
}

/* Refresh the status line from current state. Picks term_lite's
 * "active" row (last non-empty, walking from bottom up) if any,
 * otherwise the deferred-pending line (the most recent committed).
 * Brings the status line up if it isn't already. When neither has
 * any content, leave the status line down — there's nothing useful
 * to show.
 *
 * Active (vs bottom or cur) for the label: bottom is often empty
 * right after a \n leaves cursor on a fresh trailing row, and cur
 * jitters during multi-row redraws as the cursor walks up through
 * cleared rows. Active stays pinned to whatever the producer has
 * most recently painted — playwright's currently-running test line,
 * vitest's bottom-of-window summary, ninja's progress line. */
static void status_refresh(struct tool_render *r)
{
    if (!r->spinner)
        return;
    const char *text;
    size_t n;
    if (!term_lite_active_is_blank(&r->term)) {
        text = term_lite_active_data(&r->term);
        n = term_lite_active_len(&r->term);
    } else if (r->pending.len > 0) {
        text = r->pending.data;
        n = r->pending.len;
    } else {
        return;
    }
    status_paint(r, text, n);
    if (!r->status_active) {
        spinner_show(r->spinner);
        r->status_active = 1;
    }
}

/* Open the dim block on the terminal, unconditionally. Each tool-
 * output line is preceded by disp_tool_strip(), which closes its own
 * dim with ANSI_RESET so callers can apply per-line SGR (the diff
 * mode does this for color); for the head/elision/tail body we want
 * dim back, so re-emit ANSI_DIM after every strip. dim_open stays
 * 1 throughout, used by finalize to know it should emit ANSI_RESET. */
static void dim_resume(struct tool_render *r)
{
    disp_raw(ANSI_DIM);
    r->dim_open = 1;
}

/* Truncate `line` (n bytes, valid UTF-8) to one terminal row's worth
 * of cells, accounting for the gutter strip so it never wraps, and
 * write the result through disp_write. Shared between the pipe-row
 * and corner-row emitters. */
static void emit_truncated(struct tool_render *r, const char *line, size_t len)
{
    /* Reserve the gutter cells from the per-line cap so the strip
     * itself doesn't push content into a wrap. +3 in the malloc for
     * a potential "..." suffix from truncate_to_cols. */
    size_t cols = preview_cols();
    size_t budget = cols > DISP_TOOL_STRIP_COLS ? cols - DISP_TOOL_STRIP_COLS : 1;
    char *trunc = xmalloc(len + 4);
    size_t w = truncate_to_cols(line, len, budget, trunc);
    disp_write(r->disp, trunc, w);
    free(trunc);
}

/* Emit a non-final line into the dim block. The very first row of a
 * block uses "┌" (top corner); subsequent body rows use "│". The
 * trailing \n is held in disp — it'll be committed by the NEXT row's
 * disp_tool_strip → disp_write (emit_held flushes before writing), or
 * by status_paint when the spinner re-shows. */
static void emit_pipe_line(struct tool_render *r, const char *line, size_t len)
{
    if (r->first_emitted)
        disp_tool_strip(r->disp);
    else
        disp_tool_strip_first(r->disp);
    dim_resume(r);
    emit_truncated(r, line, len);
    disp_putc(r->disp, '\n');
    r->started = 1;
    r->first_emitted = 1;
}

/* Emit the final row of the dim block. If any earlier row has already
 * been emitted (multi-row block), use "└" (bottom corner); otherwise
 * this is a single-row block and we use "›" instead. Cursor is at col
 * 0 of either the just-cleared spinner row (tty) or a fresh row
 * reached via emit_held flushing the previous "│" row's trailing \n.
 * The held \n at end is preserved so the next disp_block_separator
 * can collapse the gap between this block and whatever comes next. */
static void emit_close_line(struct tool_render *r, const char *line, size_t len)
{
    if (r->first_emitted)
        disp_tool_strip_last(r->disp);
    else
        disp_tool_strip_solo(r->disp);
    dim_resume(r);
    emit_truncated(r, line, len);
    disp_putc(r->disp, '\n');
    r->started = 1;
    r->first_emitted = 1;
}

/* Push a committed non-empty line into the tail ring. The slot's
 * previous occupant (if any) is freed; only the most recent
 * TOOL_RENDER_TAIL_LINES are kept. */
static void tail_push(struct tool_render *r, const char *line, size_t len)
{
    int slot = r->tail_pos;
    free(r->tail_lines[slot]);
    r->tail_lines[slot] = xmalloc(len);
    if (len > 0)
        memcpy(r->tail_lines[slot], line, len);
    r->tail_lens[slot] = len;
    r->tail_pos = (slot + 1) % TOOL_RENDER_TAIL_LINES;
    if (r->tail_count < TOOL_RENDER_TAIL_LINES)
        r->tail_count++;
}

/* Dispatch the currently-pending line into the dim block: emit it as a
 * "│" head row, push it to the tail ring (R_HEAD_TAIL post-cap), or
 * just bump the suppressed counters (R_HEAD_ONLY post-cap). Pending is
 * consumed (len reset) regardless. Caller must have hidden the status
 * spinner when running in tty mode — emit_pipe_line writes on the
 * row the spinner was occupying. No-op when pending is empty (e.g.,
 * the very first commit, or already drained by a prior call). */
static void promote_pending(struct tool_render *r)
{
    if (r->pending.len == 0)
        return;
    if (!r->head_full) {
        emit_pipe_line(r, r->pending.data, r->pending.len);
        r->lines_emitted++;
        r->bytes_emitted += r->pending.len + 1;
        if (r->lines_emitted >= head_lines_cap(r))
            r->head_full = 1;
    } else {
        r->suppressed_lines++;
        r->suppressed_bytes += r->pending.len + 1;
        if (r->mode == R_HEAD_TAIL)
            tail_push(r, r->pending.data, r->pending.len);
    }
    buf_reset(&r->pending);
}

/* on_line callback fired by term_lite for each row that exits the
 * buffer (eviction during feed, or drain during flush). Empty /
 * whitespace-only lines are dropped from the preview — they'd waste
 * the head budget and contribute no signal. The committed line is
 * held back as the new pending; the *previous* pending is what gets
 * dispatched to the head / tail / suppressed counters. This one-line
 * lookahead is what lets the spinner show the most-recent committed
 * line live and lets finalize write "└ <pending>" in place of a
 * redundant trailing "│" row.
 *
 * has_newline: term_lite distinguishes complete rows (followed by \n)
 * from a true trailing partial. The preview doesn't care — every row
 * shown in the dim block reads the same — so we ignore the flag. */
static void on_committed_line(const char *line, size_t len, int has_newline, void *user)
{
    (void)has_newline;
    struct tool_render *r = user;

    if (bytes_are_blank(line, len))
        return;

    /* Take the spinner down so promote_pending can write its "│" row
     * on the spinner's row (tty) — without this the spinner glyph and
     * label would still be on screen when emit_pipe_line writes there. */
    int had_status = r->status_active;
    if (had_status)
        status_hide(r);

    promote_pending(r);

    /* Store the new line as the next pending. It IS the last committed
     * line — the spinner will now show it as its label, and at finalize
     * it becomes the close-row content (or contributes to footer/tail
     * counters if head_full has triggered). */
    buf_append(&r->pending, line, len);
    r->started = 1;

    /* Re-show the spinner with the new pending content. status_paint
     * commits the held \n that emit_pipe_line just buffered, so the
     * spinner lands on a fresh row below the row we just wrote. */
    if (had_status)
        status_refresh(r);
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

static void emit_diff_line(struct tool_render *r, const char *line, size_t len, int with_newline)
{
    /* First diff row gets the top corner; subsequent rows get the body
     * strip. The matching close glyph is chosen at finalize based on
     * whether we ended up with just one row or several. */
    if (r->first_emitted)
        disp_tool_strip(r->disp);
    else
        disp_tool_strip_first(r->disp);
    r->first_emitted = 1;
    r->lines_emitted++;
    const char *color = diff_line_color(line, len);
    if (color)
        disp_raw(color);
    disp_write(r->disp, line, len);
    if (color)
        disp_raw(ANSI_RESET);
    if (with_newline)
        disp_putc(r->disp, '\n');
}

static void diff_first_byte(struct tool_render *r)
{
    if (r->started)
        return;
    r->started = 1;
    if (r->spinner && !r->spinner_paused) {
        spinner_hide(r->spinner);
        r->spinner_paused = 1;
    }
}

/* Diff mode: line-buffer until \n, then emit colored. */
static void emit_byte_diff(struct tool_render *r, char ch)
{
    diff_first_byte(r);
    if (ch == '\n') {
        emit_diff_line(r, r->diff_line.data ? r->diff_line.data : "", r->diff_line.len, 1);
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
     * split at an emit_display-chunk boundary isn't double-replaced.
     * ctrl_strip now passes \r and \b through; term_lite is the
     * stage that interprets them downstream of utf8_sanitize. */
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
        /* term_lite fires on_committed_line for each \n boundary;
         * the in-progress buffer survives across chunks for the live
         * status line. */
        term_lite_feed(&r->term, out, on, on_committed_line, r);
        /* Refresh the spinner regardless — even when no full line
         * landed, the in-progress buffer may have changed (rewriting
         * progress lines) and we want the user to see the live state.
         * Note: r->started is set only by on_committed_line (when a
         * non-blank line actually commits), so a stream of pure blanks
         * doesn't trigger a close row at finalize. */
        status_refresh(r);
    }

    if (out != stack_utf8)
        free(out);
    if (clean != stack_strip)
        free(clean);
    fflush(stdout);
}

/* Render the elision marker (a "│" row) plus the tail ring for
 * R_HEAD_TAIL post-cap. The very last tail row is emitted as the
 * "└" close. Caller must have already pushed any remaining pending
 * into the ring/counters and hidden the spinner. */
static void render_tail_close(struct tool_render *r)
{
    int mid_lines = r->suppressed_lines - r->tail_count;
    size_t tail_bytes_kept = 0;
    for (int i = 0; i < r->tail_count; i++)
        tail_bytes_kept += r->tail_lens[i] + 1;
    size_t mid_bytes =
        r->suppressed_bytes > tail_bytes_kept ? r->suppressed_bytes - tail_bytes_kept : 0;

    if (mid_lines > 0 && mid_bytes > 0) {
        /* Elision marker as a regular "│" row — there's a real gap
         * between head and tail. */
        disp_tool_strip(r->disp);
        dim_resume(r);
        disp_printf(r->disp, "... (%d more line%s, %zu more byte%s) ...", mid_lines,
                    mid_lines == 1 ? "" : "s", mid_bytes, mid_bytes == 1 ? "" : "s");
        disp_putc(r->disp, '\n');
    }
    /* Walk the ring oldest-to-newest. All but the last get "│"; the
     * last is the close row, gets "└". */
    int oldest = r->tail_count == TOOL_RENDER_TAIL_LINES ? r->tail_pos : 0;
    for (int k = 0; k < r->tail_count - 1; k++) {
        int idx = (oldest + k) % TOOL_RENDER_TAIL_LINES;
        emit_pipe_line(r, r->tail_lines[idx], r->tail_lens[idx]);
    }
    int last_idx = (oldest + r->tail_count - 1) % TOOL_RENDER_TAIL_LINES;
    emit_close_line(r, r->tail_lines[last_idx], r->tail_lens[last_idx]);
}

/* Emit the "(N more lines, M more bytes)" footer for R_HEAD_ONLY post-cap
 * directly as the close row. Uses the multi-row close glyph because
 * head_full implies at least one head row has already been emitted. */
static void emit_close_footer(struct tool_render *r)
{
    disp_tool_strip_last(r->disp);
    dim_resume(r);
    disp_printf(r->disp, "... (%d more line%s, %zu more byte%s)", r->suppressed_lines,
                r->suppressed_lines == 1 ? "" : "s", r->suppressed_bytes,
                r->suppressed_bytes == 1 ? "" : "s");
    disp_putc(r->disp, '\n');
    r->started = 1;
    r->first_emitted = 1;
}

void tool_render_finalize(struct tool_render *r)
{
    /* Flush any in-progress UTF-8 sequence as U+FFFD. Drives the same
     * pipeline as live bytes — term_lite for non-diff, byte-diff
     * dispatch for R_DIFF. */
    char tail[UTF8_SANITIZE_FLUSH_MAX];
    size_t tn = utf8_sanitize_flush(&r->utf8, tail);
    if (tn > 0) {
        if (r->mode == R_DIFF) {
            for (size_t i = 0; i < tn; i++)
                emit_byte_diff(r, tail[i]);
        } else {
            term_lite_feed(&r->term, tail, tn, on_committed_line, r);
        }
    }

    if (r->mode == R_DIFF) {
        if (r->diff_line.len > 0) {
            emit_diff_line(r, r->diff_line.data, r->diff_line.len, 1);
            buf_reset(&r->diff_line);
        }
        /* Close the strip on the last diff row so the block reads as
         * a closed tree. emit_diff_line was the last write, leaving
         * the cursor on the row whose strip we'll overprint. Pick the
         * glyph based on total row count: "└" overprints the last "│"
         * for multi-row blocks; "›" overprints the lone "┌" when the
         * whole diff was just one line. */
        if (r->started) {
            if (r->lines_emitted >= 2)
                disp_tool_strip_close();
            else
                disp_tool_strip_close_solo();
        }
        if (r->spinner && !r->spinner_paused) {
            spinner_hide(r->spinner);
            r->spinner_paused = 1;
        }
        fflush(stdout);
        return;
    }

    /* Non-diff: take the status line down, then flush any in-progress
     * line through term_lite — it'll fire on_committed_line one
     * more time, which dispatches the previous pending and stores the
     * flushed buffer as the new pending. After this, pending is the
     * very last committed line (or empty if nothing ever committed). */
    status_hide(r);
    term_lite_flush(&r->term, on_committed_line, r);
    /* on_committed_line may have re-shown the spinner via status_refresh.
     * Take it down again — the close row(s) will be written on the
     * spinner's row (tty mode) or after a held-\n flush (no-tty). */
    status_hide(r);

    if (!r->started) {
        if (r->spinner && !r->spinner_paused) {
            spinner_hide(r->spinner);
            r->spinner_paused = 1;
        }
        return;
    }

    /* Dispatch the close row(s). Pending here is the most recent
     * committed line. The shape of the close depends on whether the
     * head cap was reached and which mode we're in. */
    if (!r->head_full) {
        /* Pending is the very last visible row of the dim block. */
        emit_close_line(r, r->pending.data, r->pending.len);
        buf_reset(&r->pending);
    } else if (r->mode == R_HEAD_ONLY) {
        /* Pending is the most-recent suppressed line; count it and
         * emit the footer as the close row. */
        r->suppressed_lines++;
        r->suppressed_bytes += r->pending.len + 1;
        buf_reset(&r->pending);
        emit_close_footer(r);
    } else {
        /* R_HEAD_TAIL post-cap: push pending into the tail ring (so
         * the very latest line is the last tail row), then render the
         * elision marker + tail with "└" on the final entry. */
        r->suppressed_lines++;
        r->suppressed_bytes += r->pending.len + 1;
        tail_push(r, r->pending.data, r->pending.len);
        buf_reset(&r->pending);
        render_tail_close(r);
    }

    if (r->dim_open) {
        disp_raw(ANSI_RESET);
        r->dim_open = 0;
    }
    if (r->spinner && !r->spinner_paused) {
        spinner_hide(r->spinner);
        r->spinner_paused = 1;
    }
    /* Restore the disp.held / disp.trail invariants block_separator
     * relies on. While streaming, the status-line spinner forced us
     * to flush held \n's mid-stream so its \r-positioning didn't
     * overprint pending content; that left disp.trail at the count
     * of committed lines, which would make the next block_separator
     * see "plenty of trailing newlines, add nothing" and butt the
     * next block right up against the last tail line. We instead
     * pretend the dim block ended the way the old byte-by-byte path
     * left it: trail=0, held=1 (the closing \n still buffered). The
     * cursor really is many rows below the start now, and that's
     * fine — block_separator will add 2 more \n on top of that,
     * giving exactly one blank row between this block and the next,
     * matching what users see for any other tool preview. */
    r->disp->trail = 0;
    r->disp->held = 1;
    fflush(stdout);
}

int tool_render_emit(const char *bytes, size_t n, void *user)
{
    struct tool_render *r = user;
    r->emit_called = 1;
    tool_render_feed(r, bytes, n);
    return 0;
}
