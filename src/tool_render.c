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

/* Hard cap on the in-progress line buffer. Past this the buffer stops
 * growing — the renderer only ever displays the first content_budget
 * cells of a line anyway (the rest is truncated by truncate_for_display),
 * and the tail ring is itself bounded at DISP_HT_TAIL_BYTES. The
 * accurate-byte-count counter (line_total_bytes) keeps the footer's
 * "(N more bytes)" wording correct even past the cap. */
#define LINE_BUF_CAP 4096

/* Cell budget for one row's content. Subtracts the gutter strip width
 * (DISP_TOOL_STRIP_COLS) plus a one-cell right margin — the margin
 * keeps the cursor strictly under terminal width even when a row fills
 * the budget exactly, avoiding the "phantom space" auto-wrap that some
 * terminals do at col == width. */
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
    buf_init(&r->line);
    buf_init(&r->status_content);
    buf_init(&r->diff_line);
}

void tool_render_free(struct tool_render *r)
{
    free(r->tail);
    buf_free(&r->line);
    buf_free(&r->status_content);
    buf_free(&r->diff_line);
}

/* True when the line is empty or contains only ASCII space/tab — the
 * elision policy for the live preview. The model still sees the raw
 * bytes via the tool's return string, so the elision is purely
 * visual. Used by the tail-replay path which doesn't have the live
 * line_saw_non_ws flag handy. */
static int line_is_blank(const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (c != ' ' && c != '\t')
            return 0;
    }
    return 1;
}

/* ---- Status / row painting ---- */

/* Strip glyphs in dim cyan: spinner (one cell, dynamic) for the live
 * status row, "┌" for the first permanent row, "│" for body rows,
 * marker rows, and post-status replacements. Each is followed by one
 * space to give content breathing room from the gutter. */
static void emit_first_strip(struct disp *d)
{
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x94\x8C " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

static void emit_body_strip(struct disp *d)
{
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x94\x82 " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

/* Emit the appropriate strip for the row about to be written. The
 * choice depends on whether any permanent row has been committed yet
 * — the very first emits "┌", subsequent ones "│". */
static void emit_strip_for_next_row(struct tool_render *r)
{
    if (r->rows_emitted == 0)
        emit_first_strip(r->disp);
    else
        emit_body_strip(r->disp);
}

/* Truncate `s` to fit within `cap` cells, codepoint-aware. Returns a
 * heap-allocated NUL-terminated string the caller frees. */
static char *truncate_line(const char *s, size_t cap)
{
    return truncate_for_display(s ? s : "", cap);
}

/* Hand the current status_content off to the spinner — it'll paint
 * the dim-cyan glyph + dim content row at its tick rate so the
 * animation stays alive between content updates (or, in non-tty
 * contexts where the thread doesn't run, just paints synchronously
 * once per content update). On first call this swaps the spinner
 * into SPINNER_TOOL_STATUS mode; on subsequent calls only the
 * content updates.
 *
 * Flushes any held \n (from a prior status_commit) first — the
 * spinner's draw bypasses disp's tracking, so without the flush the
 * new status would overprint the row we just committed instead of
 * landing on a fresh row below it. */
static void status_paint(struct tool_render *r)
{
    disp_emit_held(r->disp);
    const char *content = r->status_content.data ? r->status_content.data : "";
    if (!r->status_painted) {
        spinner_show_tool_status(r->spinner, content);
        r->status_painted = 1;
    } else {
        spinner_set_tool_status_content(r->spinner, content);
    }
    r->started = 1;
}

/* Convert the live status row into a permanent committed row: hide
 * the spinner (which erases the status row), emit "┌"/"│" strip +
 * status content + \n through disp. The held \n stays held — the
 * next status_paint flushes it via disp_emit_held; or if this is the
 * final commit at finalize, the held \n stays so the close-glyph
 * overprint can land on this row's strip. */
static void status_commit(struct tool_render *r)
{
    if (!r->status_painted)
        return;
    spinner_hide(r->spinner);
    emit_strip_for_next_row(r);
    disp_raw(ANSI_DIM);
    char *trimmed = truncate_line(r->status_content.data, content_budget());
    disp_write(r->disp, trimmed, strlen(trimmed));
    free(trimmed);
    disp_raw(ANSI_RESET);
    disp_putc(r->disp, '\n');
    fflush(stdout);
    r->rows_emitted++;
    r->lines_emitted++;
    r->bytes_emitted += r->status_content.len + 1;
    r->status_painted = 0;
}

/* Replace the status row with a marker row (e.g., footer or elision):
 * hide the spinner, then emit strip + marker text + held \n. Held \n
 * stays held so the close-glyph overprint at finalize can land on
 * this row's strip without an intervening blank line. */
static void status_replace_with_marker(struct tool_render *r, const char *marker)
{
    spinner_hide(r->spinner);
    emit_strip_for_next_row(r);
    disp_raw(ANSI_DIM);
    char *trimmed = truncate_line(marker, content_budget());
    disp_write(r->disp, trimmed, strlen(trimmed));
    free(trimmed);
    disp_raw(ANSI_RESET);
    disp_putc(r->disp, '\n');
    fflush(stdout);
    r->rows_emitted++;
    r->status_painted = 0;
    r->started = 1;
}

/* Drop the live status row without emitting anything new in its place:
 * hide the spinner (which erases its current paint), cursor lands at
 * col 0 of the cleared row. Subsequent strip emits land on this row,
 * so the cleared status becomes the next permanent row's slot. Used
 * at finalize when the tail-fits-inline path is about to replay tail
 * rows that would duplicate / supersede the status content. */
static void status_drop(struct tool_render *r)
{
    if (!r->status_painted)
        return;
    spinner_hide(r->spinner);
    r->status_painted = 0;
}

/* Emit a freshly-built permanent row with the given content. Used by
 * tail replay at finalize. Truncation drops the right end (keeping the
 * line start) — same direction as head rows and live streaming, so a
 * reader scanning the block left-to-right always sees consistent line
 * starts. Held \n stays held so the close-glyph overprint can land on
 * the last row. */
static void emit_permanent_row(struct tool_render *r, const char *content, size_t len)
{
    emit_strip_for_next_row(r);
    disp_raw(ANSI_DIM);
    /* truncate_for_display takes NUL-terminated input — copy into a
     * temporary buffer since `content` is a slice. */
    char *tmp = xmalloc(len + 1);
    memcpy(tmp, content, len);
    tmp[len] = 0;
    char *trimmed = truncate_for_display(tmp, content_budget());
    free(tmp);
    disp_write(r->disp, trimmed, strlen(trimmed));
    free(trimmed);
    disp_raw(ANSI_RESET);
    disp_putc(r->disp, '\n');
    r->rows_emitted++;
    r->started = 1;
}

/* ---- Suppression / tail ring ---- */

static void tail_push_byte(struct tool_render *r, char ch)
{
    if (r->mode != R_HEAD_TAIL)
        return;
    r->tail[r->tail_pos++] = ch;
    if (r->tail_pos == DISP_HT_TAIL_BYTES) {
        r->tail_pos = 0;
        r->tail_wrapped = 1;
    }
    /* Track display-byte counts for the suppression slice math.
     * line_display_bytes resets at \n boundaries; suppressed_display_
     * bytes accumulates for the lifetime of the block. The head_full
     * gate undercounts the cap-tripping line — process_line
     * compensates retroactively at the transition. */
    r->line_display_bytes++;
    if (r->head_full)
        r->suppressed_display_bytes++;
}

/* Account one suppressed line for the elision marker / footer line
 * count. Blank lines are silently elided everywhere (head, tail, pre-
 * cap), so they don't appear in this counter either — passing
 * blank=1 is a no-op. An unterminated trailing line counts as one
 * line when it's non-blank.
 *
 * The actual byte-level work (ring push, suppressed_display_bytes
 * bookkeeping) happens inline in tool_render_feed via tail_push_byte;
 * suppress_account only handles the user-facing line counter. */
static void suppress_account(struct tool_render *r, int blank)
{
    if (blank)
        return;
    r->suppressed_lines += 1;
}

/* ---- Per-line classification (HEAD_ONLY / HEAD_TAIL) ---- */

/* True when the head cap has been reached after the most recent
 * commit — drives the head_full transition so subsequent lines
 * (blank or otherwise) get routed through suppression. */
static int head_at_cap(const struct tool_render *r)
{
    return (size_t)r->lines_emitted >= (size_t)head_lines_cap(r) ||
           r->bytes_emitted >= head_bytes_cap(r);
}

/* True when committing the currently painted status would put the
 * committed row count at or past the head cap. Used by the blank-
 * line path to decide whether to commit the visible status now (so
 * the line content survives) and start counting subsequent input as
 * suppression — without this, blanks arriving when the visual head
 * is "full but not yet committed" would silently bypass the cap. */
static int status_commit_would_fill_head(const struct tool_render *r)
{
    if (!r->status_painted)
        return 0;
    size_t post_lines = (size_t)r->lines_emitted + 1;
    size_t post_bytes = r->bytes_emitted + r->status_content.len + 1;
    return post_lines >= (size_t)head_lines_cap(r) || post_bytes >= head_bytes_cap(r);
}

/* Process one logical line. `bytes`/`len` is the cap-truncated line
 * buffer (may be missing a tail past LINE_BUF_CAP); blank says
 * whether any non-ws codepoint was seen; has_terminator distinguishes
 * normal feed-loop calls (1) from the trailing-partial drain at
 * finalize (0). The total-byte parameter is no longer needed since
 * the marker text is line-only. */
static void process_line(struct tool_render *r, const char *bytes, size_t len, int blank,
                         int has_terminator)
{
    (void)has_terminator;
    if (blank) {
        /* Blank lines are silently elided from the preview at every
         * level — head, tail, and the suppression counters. The only
         * thing they do past the cap is contribute their byte to the
         * tail ring (already handled via tail_push_byte) so the ring
         * slice math stays consistent. The "visual head full but not
         * yet committed" case still needs handling here: when a blank
         * arrives with the painted status sitting on the cap row,
         * commit the status now and transition to head_full so any
         * subsequent non-blank content lands in suppression. */
        if (!r->head_full && status_commit_would_fill_head(r)) {
            status_commit(r);
            r->head_full = 1;
            /* The blank line's bytes were pushed to the ring before
             * head_full flipped, so they were never credited to
             * suppressed_display_bytes by tail_push_byte. Compensate
             * now so build_tail_view's slice math accounts for them. */
            r->suppressed_display_bytes += r->line_display_bytes;
        }
        return;
    }
    /* Non-blank line. Pre-cap: commit the previous status as a
     * permanent row before showing the new one. After commit, check
     * whether the row counters tripped the cap — if so, this new line
     * goes into suppression too. */
    if (r->status_painted && !r->head_full) {
        status_commit(r);
        if (head_at_cap(r)) {
            r->head_full = 1;
            /* Cap-tripping line's bytes are already in the ring but
             * weren't counted under head_full=0; credit them now. */
            r->suppressed_display_bytes += r->line_display_bytes;
        }
    }
    /* Truncate the line to one row's worth of cells before storing —
     * the status_content buffer doesn't need to keep more than what
     * one repaint will display, and the truncation marker (if any) is
     * baked in by truncate_for_display. */
    char *tmp = xmalloc(len + 1);
    memcpy(tmp, bytes, len);
    tmp[len] = 0;
    char *trimmed = truncate_line(tmp, content_budget());
    free(tmp);
    buf_reset(&r->status_content);
    buf_append(&r->status_content, trimmed, strlen(trimmed));
    free(trimmed);
    status_paint(r);
    /* Once head is full, every non-blank line also feeds the
     * suppression line counter — the status row floats above whatever
     * the tool emits, while finalize uses the tail ring to compose
     * the actual visible aftermath. The ring itself is filled inline
     * by tool_render_feed regardless of LINE_BUF_CAP. */
    if (r->head_full)
        suppress_account(r, /* blank */ 0);
}

/* ---- Diff mode (unchanged from before — line-buffered, colored) ---- */

static const char *diff_line_color(const char *line, size_t len)
{
    if (len >= 4 && (memcmp(line, "--- ", 4) == 0 || memcmp(line, "+++ ", 4) == 0))
        return ANSI_DIM;
    if (len >= 2 && memcmp(line, "@@", 2) == 0)
        return ANSI_DIM;
    if (len >= 1 && line[0] == '+')
        return ANSI_GREEN;
    if (len >= 1 && line[0] == '-')
        return ANSI_RED;
    if (len >= 1 && line[0] == '\\')
        return ANSI_DIM;
    return NULL;
}

static void emit_diff_line(struct tool_render *r, const char *line, size_t len)
{
    /* On the first row of the diff block, hide whatever the spinner
     * is currently showing (the agent may have left it in inline /
     * line mode from before dispatch) so it doesn't fight the row
     * paint. Subsequent diff rows skip the hide — the renderer
     * doesn't bring the spinner back during R_DIFF, so once it's
     * off it stays off. */
    if (!r->started)
        spinner_hide(r->spinner);
    emit_strip_for_next_row(r);
    const char *color = diff_line_color(line, len);
    if (color)
        disp_raw(color);
    char *trimmed = truncate_line(line, content_budget());
    disp_write(r->disp, trimmed, strlen(trimmed));
    free(trimmed);
    if (color)
        disp_raw(ANSI_RESET);
    disp_putc(r->disp, '\n');
    r->rows_emitted++;
    r->started = 1;
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

/* ---- Public feed/finalize ---- */

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
                /* Push the line-terminating \n to the tail ring
                 * before process_line. The ring captures every byte
                 * (head bytes get overwritten by tail bytes once the
                 * ring fills); build_tail_view at finalize extracts
                 * just the suppressed-portion slice using
                 * suppressed_bytes as the length. tail_push_byte is a
                 * no-op for non-HEAD_TAIL modes. */
                tail_push_byte(r, '\n');
                process_line(r, r->line.data ? r->line.data : "", r->line.len, !r->line_saw_non_ws,
                             /* has_terminator */ 1);
                buf_reset(&r->line);
                r->line_total_bytes = 0;
                r->line_display_bytes = 0;
                r->line_saw_non_ws = 0;
                i++;
                continue;
            }
            /* Tab is the one C0 control ctrl_strip preserves; pass it
             * through as one cell of whitespace. utf8_codepoint_cells
             * treats it as dangerous (returns -1), so it must skip the
             * codepoint walk below to avoid being substituted with
             * "?". Tabs don't set line_saw_non_ws — they're whitespace
             * for elision purposes. */
            if (c == '\t') {
                if (r->line.len < LINE_BUF_CAP)
                    buf_append(&r->line, "\t", 1);
                tail_push_byte(r, '\t');
                r->line_total_bytes++;
                i++;
                continue;
            }
            /* Walk by codepoint so dangerous ones (Trojan Source bidi
             * overrides, stray format chars, etc.) can be substituted
             * with "?" before they reach the terminal. utf8_sanitize
             * already replaced malformed sequences with U+FFFD, so
             * cells < 0 here means a well-formed codepoint that
             * wcwidth / codepoint_is_dangerous flagged. */
            size_t consumed = 0;
            int cells = utf8_codepoint_cells(out, on, i, &consumed);
            if (consumed == 0)
                consumed = 1;
            const char *append_bytes;
            size_t append_len;
            int is_substituted = (cells < 0);
            if (is_substituted) {
                append_bytes = "?";
                append_len = 1;
            } else {
                append_bytes = out + i;
                append_len = consumed;
            }
            /* Append only when the whole codepoint fits within the
             * line buffer cap — never split a multi-byte sequence. */
            if (r->line.len + append_len <= LINE_BUF_CAP)
                buf_append(&r->line, append_bytes, append_len);
            /* Push the (possibly substituted) bytes into the tail
             * ring unconditionally. The ring is sized for the visual
             * tail; head bytes that land here early get overwritten
             * by suppressed tail bytes once the ring fills. At
             * finalize, build_tail_view uses suppressed_bytes to
             * extract just the suppressed slice from the ring's
             * recent end. */
            for (size_t k = 0; k < append_len; k++)
                tail_push_byte(r, append_bytes[k]);
            /* line_total_bytes tracks the original byte count for the
             * suppression footer; substitution doesn't change it. */
            r->line_total_bytes += consumed;
            /* Substituted "?" is non-ws; ASCII space stays whitespace.
             * Multi-byte codepoints have a leading byte ≥ 0xC0 which
             * trivially fails the space/tab check. */
            if (is_substituted || (out[i] != ' ' && out[i] != '\t'))
                r->line_saw_non_ws = 1;
            i += consumed;
        }
    }

    if (out != stack_utf8)
        free(out);
    if (clean != stack_strip)
        free(clean);
    fflush(stdout);
}

/* Walk the tail ring contiguously into a linear buffer, back-walking
 * to keep at most DISP_HT_TAIL_LINES non-blank lines, and return the
 * resulting span plus the count of non-blank suppressed lines that
 * fell before the kept span (the "elided middle"). */
struct tail_view {
    char buf[DISP_HT_TAIL_BYTES];
    size_t kept;
    int mid_lines;
};

static void build_tail_view(const struct tool_render *r, struct tail_view *v)
{
    /* Linearize the ring: oldest byte first, newest last. */
    size_t linear_len = r->tail_wrapped ? DISP_HT_TAIL_BYTES : r->tail_pos;
    size_t oldest = r->tail_wrapped ? r->tail_pos : 0;
    char linear[DISP_HT_TAIL_BYTES];
    for (size_t k = 0; k < linear_len; k++)
        linear[k] = r->tail[(oldest + k) % DISP_HT_TAIL_BYTES];

    /* Extract just the suppressed slice from the ring's recent end.
     * The ring captures every byte the renderer sees (head + tail);
     * for short inputs head bytes still occupy the ring's start, but
     * suppressed_display_bytes tells us exactly how many of the most-
     * recent bytes are suppressed display content. Using
     * suppressed_bytes (original input byte count) here would over-
     * extend the slice into preceding head bytes when dangerous
     * codepoints were substituted (3-byte U+202E becomes 1-byte "?"
     * in the ring, but suppressed_bytes credits the original 3). */
    size_t suppressed_in_ring =
        r->suppressed_display_bytes < linear_len ? r->suppressed_display_bytes : linear_len;
    char *tail_only = linear + (linear_len - suppressed_in_ring);

    /* Back-walk through the suppressed slice to keep at most
     * DISP_HT_TAIL_LINES *non-blank* lines. Counting raw \n
     * boundaries here would let blank lines consume the tail-line
     * cap (emit_tail_rows elides them), leaving fewer visible rows
     * than the cap promises and inconsistent with the head's blank-
     * line elision. Cross blanks for free until we've counted enough
     * non-blank lines. */
    size_t tail_start = suppressed_in_ring;
    if (tail_start > 0 && tail_only[tail_start - 1] == '\n')
        tail_start--;
    /* cur_line_end tracks the end of the line we're scanning back
     * through right now (the line starting at tail_start). */
    size_t cur_line_end = tail_start;
    int kept_visible = 0;
    while (tail_start > 0) {
        tail_start--;
        if (tail_only[tail_start] == '\n') {
            size_t line_start = tail_start + 1;
            if (cur_line_end > line_start &&
                !line_is_blank(tail_only + line_start, cur_line_end - line_start)) {
                if (++kept_visible == DISP_HT_TAIL_LINES) {
                    tail_start = line_start;
                    break;
                }
            }
            cur_line_end = tail_start;
        }
    }
    /* If the slice begins mid-codepoint (the ring wrapped inside a
     * multi-byte sequence and the back-walk didn't find enough line
     * breaks to land on a \n boundary), advance past leading UTF-8
     * continuation bytes so emit_tail_rows starts on a valid lead
     * byte. The orphan partial codepoint effectively joins the
     * elided middle — a stray 0x80–0xBF byte reaching the terminal
     * would render as garbage. */
    while (tail_start < suppressed_in_ring &&
           ((unsigned char)tail_only[tail_start] & 0xC0) == 0x80) {
        tail_start++;
    }
    size_t kept = suppressed_in_ring - tail_start;
    /* Count non-blank lines in the kept span — those are what the
     * user sees rendered. Blank lines in the span are dropped by
     * emit_tail_rows, so they belong to the elided portion for
     * marker-counting purposes. */
    int mid_lines = r->suppressed_lines;
    size_t span_line_start = tail_start;
    for (size_t k = tail_start; k < suppressed_in_ring; k++) {
        if (tail_only[k] == '\n') {
            if (k > span_line_start &&
                !line_is_blank(tail_only + span_line_start, k - span_line_start))
                mid_lines--;
            span_line_start = k + 1;
        }
    }
    if (span_line_start < suppressed_in_ring &&
        !line_is_blank(tail_only + span_line_start, suppressed_in_ring - span_line_start))
        mid_lines--;
    if (mid_lines < 0)
        mid_lines = 0;

    memcpy(v->buf, tail_only + tail_start, kept);
    v->kept = kept;
    v->mid_lines = mid_lines;
}

/* Emit kept tail bytes as permanent rows, eliding empty / ws-only
 * lines on the way through (matching the live-path policy). */
static void emit_tail_rows(struct tool_render *r, const char *bytes, size_t len)
{
    struct buf line;
    buf_init(&line);
    for (size_t i = 0; i < len; i++) {
        char c = bytes[i];
        if (c == '\n') {
            const char *p = line.data ? line.data : "";
            if (!line_is_blank(p, line.len))
                emit_permanent_row(r, p, line.len);
            buf_reset(&line);
        } else {
            buf_append(&line, &c, 1);
        }
    }
    /* Trailing partial line: render if non-blank. */
    if (line.len > 0) {
        const char *p = line.data;
        if (!line_is_blank(p, line.len))
            emit_permanent_row(r, p, line.len);
    }
    buf_free(&line);
}

void tool_render_finalize(struct tool_render *r)
{
    /* Flush any in-progress UTF-8 sequence as U+FFFD. */
    char tail[UTF8_SANITIZE_FLUSH_MAX];
    size_t tn = utf8_sanitize_flush(&r->utf8, tail);
    if (tn > 0)
        tool_render_feed(r, tail, tn);

    /* Drain any in-progress line that didn't end in \n. */
    if (r->mode == R_DIFF) {
        if (r->diff_line.len > 0) {
            emit_diff_line(r, r->diff_line.data, r->diff_line.len);
            buf_reset(&r->diff_line);
        }
    } else if (r->line_total_bytes > 0) {
        /* Trailing partial line (no \n). has_terminator=0 so
         * the trailing partial counts as one line (if non-blank) via
         * the same suppress_account path used by terminated lines —
         * has_terminator=0 just signals "no synthesized \n in the
         * ring," not a different counting policy. */
        process_line(r, r->line.data ? r->line.data : "", r->line.len, !r->line_saw_non_ws,
                     /* has_terminator */ 0);
        buf_reset(&r->line);
        r->line_total_bytes = 0;
        r->line_display_bytes = 0;
        r->line_saw_non_ws = 0;
    }

    if (!r->started) {
        /* Make sure the spinner isn't lingering even when nothing
         * visible was emitted (e.g., the tool emitted only blank
         * lines). */
        spinner_hide(r->spinner);
        return;
    }

    /* Reach here with status_painted possibly still 1; the branches
     * below close the block by hiding the spinner via status_commit /
     * status_replace_with_marker / status_drop. R_DIFF has no status
     * row, so we hide explicitly. */
    if (r->mode == R_DIFF) {
        spinner_hide(r->spinner);
    }

    if (r->mode != R_DIFF) {
        /* Two finalize states need handling: status_painted=1 (the
         * common case — last non-blank line is showing as the live
         * status) and status_painted=0 with head_full=1 (the eager-
         * commit-on-blank path: the cap was tripped by a blank line
         * arriving on the cap row, status was committed and the
         * spinner cleared, but suppression has data to summarize).
         * status_replace_with_marker handles both — when status isn't
         * painted, spinner_hide is a no-op and the marker just lands
         * on a fresh row after the prior commit's held \n flushes. */
        if (r->status_painted && !r->head_full) {
            /* Under-cap, no suppression — commit the status as the
             * final permanent row. */
            status_commit(r);
        } else if (r->head_full) {
            if (r->mode == R_HEAD_ONLY) {
                /* Marker counts only renderable (non-blank) lines —
                 * blank lines are silently elided everywhere, so
                 * counting them in the footer would be misleading
                 * ("1 more line" pointing at content the user wasn't
                 * going to see anyway). Skip the marker entirely
                 * when there's nothing user-meaningful to report
                 * (e.g., commit-on-blank with no non-blank lines
                 * past the cap). */
                if (r->suppressed_lines > 0) {
                    char marker[96];
                    snprintf(marker, sizeof(marker), "... (%d more line%s)", r->suppressed_lines,
                             r->suppressed_lines == 1 ? "" : "s");
                    status_replace_with_marker(r, marker);
                }
            } else { /* R_HEAD_TAIL */
                struct tail_view v;
                build_tail_view(r, &v);
                /* Marker fires only when the tail view actually
                 * elided something user-visible (a non-blank line we
                 * would have rendered if not for the cap). */
                if (v.mid_lines > 0) {
                    char marker[96];
                    snprintf(marker, sizeof(marker), "... (%d more line%s) ...", v.mid_lines,
                             v.mid_lines == 1 ? "" : "s");
                    status_replace_with_marker(r, marker);
                    if (v.kept > 0)
                        emit_tail_rows(r, v.buf, v.kept);
                } else {
                    /* Tail fits inline — drop any live status and
                     * replay the tail rows in its place. The replay
                     * covers the same content the status was hinting
                     * at, plus any earlier suppressed lines that fit. */
                    status_drop(r);
                    if (v.kept > 0)
                        emit_tail_rows(r, v.buf, v.kept);
                }
            }
        }
    }

    /* Overprint the most recently emitted strip with the close glyph.
     * "└" for multi-row blocks, "›" for a block that ended up with
     * only one row — promotes the leading "┌" to a self-contained
     * chevron. */
    if (r->rows_emitted >= 2)
        disp_tool_strip_close();
    else if (r->rows_emitted == 1)
        disp_tool_strip_close_solo();

    fflush(stdout);

    /* Make finalize idempotent: clear started so a second call
     * short-circuits at the early-return above instead of re-emitting
     * the close glyph (which would overprint whatever the cursor now
     * points at). rows_emitted stays — callers may still inspect it
     * after finalize for accounting. */
    r->started = 0;
}

int tool_render_emit(const char *bytes, size_t n, void *user)
{
    struct tool_render *r = user;
    r->emit_called = 1;
    tool_render_feed(r, bytes, n);
    return 0;
}
