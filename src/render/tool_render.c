/* SPDX-License-Identifier: MIT */
#include "render/tool_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "util.h"
#include "render/ctrl_strip.h"
#include "render/diff_color.h"
#include "render/disp.h"
#include "render/spinner.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "terminal/width.h"
#include "text/utf8.h"
#include "text/utf8_sanitize.h"
#include "text/width.h"

#define TAIL_RING_CAPACITY 1500

static const char GUTTER_FIRST[] = "\xE2\x94\x8C";  /* ┌ */
static const char GUTTER_BODY[] = "\xE2\x94\x82";   /* │ */
static const char GUTTER_LAST[] = "\xE2\x94\x94";   /* └ */
static const char GUTTER_MARKER[] = "\xE2\x80\xBA"; /* › */

struct preview_limits {
    int head_lines;
    size_t head_bytes;
    int tail_lines;
};

static const struct preview_limits HEAD_PREVIEW_LIMITS = {
    .head_lines = 8,
    .head_bytes = 3000,
};

#define TAIL_PREVIEW_LINES 4

/* Command failures commonly end with their most useful context. */
static const struct preview_limits HEAD_TAIL_PREVIEW_LIMITS = {
    .head_lines = 4,
    .head_bytes = 1500,
    .tail_lines = TAIL_PREVIEW_LINES,
};

static const struct preview_limits *preview_limits_for_mode(enum tool_render_mode mode)
{
    return mode == TOOL_RENDER_HEAD_TAIL ? &HEAD_TAIL_PREVIEW_LIMITS : &HEAD_PREVIEW_LIMITS;
}

/* Only the beginning of a line can reach the display; the tail ring is bounded separately. */
#define LINE_BUF_CAP 4096

/* Filling the final terminal column can trigger deferred autowrap on the next write. */
static size_t row_content_budget(void)
{
    int width = display_width();
    if (width <= TOOL_RENDER_GUTTER_COLS + 5)
        return 1;
    return (size_t)(width - TOOL_RENDER_GUTTER_COLS - 1);
}

void tool_render_init(struct tool_render *render, struct disp *disp, struct spinner *spinner,
                      enum tool_render_mode mode)
{
    memset(render, 0, sizeof(*render));
    render->disp = disp;
    render->spinner = spinner;
    ctrl_strip_init(&render->strip);
    utf8_sanitizer_init(&render->sanitizer);
    render->mode = mode;
    if (mode == TOOL_RENDER_HEAD_TAIL)
        render->tail = xmalloc(TAIL_RING_CAPACITY);
    buf_init(&render->line);
    buf_init(&render->status_line);
    buf_init(&render->diff_line);
}

void tool_render_free(struct tool_render *render)
{
    free(render->tail);
    buf_free(&render->line);
    buf_free(&render->status_line);
    buf_free(&render->diff_line);
}

void tool_render_set_mode(struct tool_render *render, enum tool_render_mode mode)
{
    if (render->mode == mode)
        return;
    if (mode == TOOL_RENDER_HEAD_TAIL) {
        render->tail = xmalloc(TAIL_RING_CAPACITY);
    } else if (render->mode == TOOL_RENDER_HEAD_TAIL) {
        free(render->tail);
        render->tail = NULL;
    }
    render->mode = mode;
}

static int line_is_blank(const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char byte = bytes[i];
        if (byte != ' ' && byte != '\t')
            return 0;
    }
    return 1;
}

static void write_gutter(struct disp *disp, const char *glyph)
{
    disp_commit_newlines(disp);
    disp_write_ansi(disp, theme_open(THEME_CHROME_DIM));
    disp_write(disp, glyph, strlen(glyph));
    disp_putc(disp, ' ');
    disp_write_ansi(disp, ANSI_RESET);
}

void tool_render_write_marker_gutter(struct disp *disp)
{
    write_gutter(disp, GUTTER_MARKER);
}

/* The one definition of a preview row's look; settled rows and live spinner rows share it. */
static void compose_row(struct buf *out, const char *gutter_glyph, const char *content)
{
    buf_append_str(out, theme_open(THEME_CHROME_DIM));
    buf_append_str(out, gutter_glyph);
    buf_append_str(out, " " ANSI_RESET ANSI_DIM);
    buf_append_str(out, content);
    buf_append_str(out, ANSI_RESET);
}

static void write_next_row_gutter(struct tool_render *render)
{
    write_gutter(render->disp, render->rows_emitted == 0 ? GUTTER_FIRST : GUTTER_BODY);
}

/* The final newline remains pending, so a carriage return can replace the row's first cell. */
static void overprint_final_gutter(struct disp *disp, const char *glyph)
{
    FILE *sink = disp_sink(disp);
    fputc('\r', sink);
    fputs(theme_open(THEME_CHROME_DIM), sink);
    fputs(glyph, sink);
    fputs(ANSI_RESET, sink);
    fflush(sink);
}

static char *truncate_slice(const char *bytes, size_t len)
{
    char *copy = xmalloc(len + 1);
    memcpy(copy, bytes, len);
    copy[len] = '\0';
    char *truncated = truncate_for_display(copy, row_content_budget());
    free(copy);
    return truncated;
}

/* Live rows must also fit the physical terminal; a wrapped row breaks the spinner's row
 * accounting. */
static size_t live_row_budget(void)
{
    int width = term_width();
    if (width <= TOOL_RENDER_GUTTER_COLS + 5)
        return 1;
    size_t terminal_budget = (size_t)(width - TOOL_RENDER_GUTTER_COLS - 1);
    size_t display_budget = row_content_budget();
    return terminal_budget < display_budget ? terminal_budget : display_budget;
}

/* Spinner drawing bypasses disp, so release a pending newline before handing it the view. The
 * spinner overprints its glyph on the last row's gutter. */
static void paint_live_rows(struct tool_render *render, const char *const *contents, int count)
{
    struct buf styled[1 + TAIL_PREVIEW_LINES];
    struct spinner_row rows[1 + TAIL_PREVIEW_LINES];
    size_t budget = live_row_budget();

    for (int row = 0; row < count; row++) {
        char *clipped = truncate_for_display(contents[row], budget);
        buf_init(&styled[row]);
        compose_row(&styled[row], GUTTER_BODY, clipped);
        rows[row].bytes = styled[row].data;
        rows[row].cells = TOOL_RENDER_GUTTER_COLS + (int)display_cells(clipped);
        free(clipped);
    }

    disp_commit_newlines(render->disp);
    spinner_set_tool_status_view(render->spinner, rows, count);
    for (int row = 0; row < count; row++)
        buf_free(&styled[row]);
    render->status_visible = 1;
}

static void paint_status(struct tool_render *render)
{
    const char *content = render->status_line.data ? render->status_line.data : "";
    paint_live_rows(render, &content, 1);
    render->status_placeholder = 0;
    render->block_open = 1;
}

void tool_render_begin_live(struct tool_render *render)
{
    if (!render->spinner)
        return;
    const char *placeholder = "";
    paint_live_rows(render, &placeholder, 1);
    render->status_placeholder = 1;
}

static void emit_row(struct tool_render *render, const char *content, size_t len)
{
    char *truncated = truncate_slice(content, len);
    struct buf row;
    buf_init(&row);
    compose_row(&row, render->rows_emitted == 0 ? GUTTER_FIRST : GUTTER_BODY, truncated);
    free(truncated);
    disp_commit_newlines(render->disp);
    disp_write(render->disp, row.data, row.len);
    buf_free(&row);
    disp_putc(render->disp, '\n');
    render->rows_emitted++;
    render->block_open = 1;
}

/* Erasing the live status and committing its settled row spans two writers; the swap bracket
 * makes them render as one frame, not blank-and-repaint. */
static void commit_status(struct tool_render *render)
{
    if (!render->status_visible)
        return;
    spinner_swap_begin(render->spinner);
    emit_row(render, render->status_line.data ? render->status_line.data : "",
             render->status_line.len);
    spinner_swap_end(render->spinner);
    disp_flush(render->disp);
    render->head_lines_emitted++;
    render->head_bytes_emitted += render->status_line.len + 1;
    render->status_visible = 0;
}

/* Part of the finalize swap: the bracket opened with the erase closes at the end of finalize,
 * after the tail rows. */
static void replace_status_with_marker(struct tool_render *render, const char *marker)
{
    spinner_swap_begin(render->spinner);
    emit_row(render, marker, strlen(marker));
    disp_flush(render->disp);
    render->status_visible = 0;
}

static void drop_status(struct tool_render *render)
{
    if (!render->status_visible)
        return;
    spinner_swap_begin(render->spinner);
    render->status_visible = 0;
}

static void push_tail_byte(struct tool_render *render, char byte)
{
    if (render->mode != TOOL_RENDER_HEAD_TAIL)
        return;
    render->tail[render->tail_write_pos++] = byte;
    if (render->tail_write_pos == TAIL_RING_CAPACITY) {
        render->tail_write_pos = 0;
        render->tail_full = 1;
    }
    render->line_tail_bytes++;
    if (render->head_complete)
        render->suppressed_tail_bytes++;
}

static int head_limit_reached(const struct tool_render *render)
{
    const struct preview_limits *limits = preview_limits_for_mode(render->mode);
    return (size_t)render->head_lines_emitted >= (size_t)limits->head_lines ||
           render->head_bytes_emitted >= limits->head_bytes;
}

/* The live status is not included in head counters until it is committed. */
static int status_reaches_head_limit(const struct tool_render *render)
{
    if (!render->status_visible)
        return 0;
    const struct preview_limits *limits = preview_limits_for_mode(render->mode);
    size_t head_lines_after_commit = (size_t)render->head_lines_emitted + 1;
    size_t head_bytes_after_commit = render->head_bytes_emitted + render->status_line.len + 1;
    return head_lines_after_commit >= (size_t)limits->head_lines ||
           head_bytes_after_commit >= limits->head_bytes;
}

static void mark_head_complete(struct tool_render *render)
{
    render->head_complete = 1;
    /* The current line entered the ring before the renderer discovered that the head was full. */
    render->suppressed_tail_bytes += render->line_tail_bytes;
}

static void paint_live_tail(struct tool_render *render, const char *bytes, size_t len);

static void process_line(struct tool_render *render, const char *bytes, size_t len, int is_blank)
{
    if (is_blank) {
        if (!render->head_complete && !render->status_placeholder &&
            status_reaches_head_limit(render)) {
            commit_status(render);
            mark_head_complete(render);
        }
        return;
    }

    /* The placeholder is replaced in place by the first line, never committed as a row. */
    if (render->status_visible && !render->status_placeholder && !render->head_complete) {
        commit_status(render);
        if (head_limit_reached(render))
            mark_head_complete(render);
    }

    /* status_line always holds the newest completed line: the pending head row before the cap,
     * the newest tail line after it (whose start the byte-bounded tail ring may have lost). */
    char *truncated = truncate_slice(bytes, len);
    buf_reset(&render->status_line);
    buf_append(&render->status_line, truncated, strlen(truncated));
    free(truncated);

    if (render->head_complete && render->mode == TOOL_RENDER_HEAD_TAIL && render->spinner) {
        render->suppressed_lines++;
        paint_live_tail(render, bytes, len);
        return;
    }

    paint_status(render);
    if (render->head_complete)
        render->suppressed_lines++;
}

static const char *diff_line_color(const char *line, size_t len, int hunk_started)
{
    switch (diff_line_classify(line, len, hunk_started)) {
    case DIFF_LINE_ADD:
        return theme_open(THEME_ADD);
    case DIFF_LINE_REMOVE:
        return theme_open(THEME_REMOVE);
    case DIFF_LINE_META:
    case DIFF_LINE_CONTEXT:
        break;
    }
    return ANSI_DIM;
}

static void emit_diff_line(struct tool_render *render, const char *line, size_t len)
{
    /* The tool-call header already identifies the file. */
    if (!render->diff_hunk_started && diff_is_file_header(line, len))
        return;
    /* Replacing the spinner (or the live placeholder) with the first row is a swap, so it
     * cannot render as blank-and-repaint. */
    int opening = !render->block_open;
    if (opening) {
        spinner_swap_begin(render->spinner);
        render->status_visible = 0;
        render->status_placeholder = 0;
    }
    write_next_row_gutter(render);
    disp_write_ansi(render->disp, diff_line_color(line, len, render->diff_hunk_started));
    char *truncated = truncate_slice(line, len);
    disp_write(render->disp, truncated, strlen(truncated));
    free(truncated);
    disp_write_ansi(render->disp, ANSI_RESET);
    disp_putc(render->disp, '\n');
    if (opening)
        spinner_swap_end(render->spinner);
    render->rows_emitted++;
    render->block_open = 1;
    /* Inside a hunk, content resembling a file header must still be classified by its prefix. */
    if (len >= 2 && memcmp(line, "@@", 2) == 0)
        render->diff_hunk_started = 1;
}

static void feed_diff_byte(struct tool_render *render, char byte)
{
    if (byte == '\n') {
        emit_diff_line(render, render->diff_line.data ? render->diff_line.data : "",
                       render->diff_line.len);
        buf_reset(&render->diff_line);
    } else if (byte == '\t') {
        buf_append(&render->diff_line, "    ", 4);
    } else {
        buf_append(&render->diff_line, &byte, 1);
    }
}

static void reset_line(struct tool_render *render)
{
    buf_reset(&render->line);
    render->line_tail_bytes = 0;
    render->line_has_non_whitespace = 0;
}

void tool_render_feed(struct tool_render *render, const char *bytes, size_t len)
{
    if (len == 0)
        return;

    char stack_stripped[4096];
    char *stripped = len <= sizeof(stack_stripped) ? stack_stripped : xmalloc(len);
    size_t stripped_len = ctrl_strip_feed(&render->strip, bytes, len, stripped);

    char stack_sanitized[UTF8_SANITIZE_FEED_MAX(4096)];
    size_t sanitized_cap = UTF8_SANITIZE_FEED_MAX(stripped_len);
    char *sanitized =
        sanitized_cap <= sizeof(stack_sanitized) ? stack_sanitized : xmalloc(sanitized_cap);
    size_t sanitized_len =
        utf8_sanitizer_feed(&render->sanitizer, stripped, stripped_len, sanitized);

    if (render->mode == TOOL_RENDER_DIFF) {
        for (size_t i = 0; i < sanitized_len; i++)
            feed_diff_byte(render, sanitized[i]);
    } else {
        size_t offset = 0;
        while (offset < sanitized_len) {
            char byte = sanitized[offset];
            if (byte == '\n') {
                push_tail_byte(render, '\n');
                process_line(render, render->line.data ? render->line.data : "", render->line.len,
                             !render->line_has_non_whitespace);
                reset_line(render);
                offset++;
                continue;
            }

            /* Terminal tab stops do not match the width calculation used for truncation. */
            if (byte == '\t') {
                static const char TAB_SPACES[] = "    ";
                size_t tab_len = sizeof(TAB_SPACES) - 1;
                if (render->line.len + tab_len <= LINE_BUF_CAP)
                    buf_append(&render->line, TAB_SPACES, tab_len);
                for (size_t i = 0; i < tab_len; i++)
                    push_tail_byte(render, TAB_SPACES[i]);
                offset++;
                continue;
            }

            size_t codepoint_len = 0;
            int cells = utf8_codepoint_cells(sanitized, sanitized_len, offset, &codepoint_len);
            if (codepoint_len == 0)
                codepoint_len = 1;

            const char *display_bytes = sanitized + offset;
            size_t display_len = codepoint_len;
            int substituted = cells < 0;
            if (substituted) {
                display_bytes = "?";
                display_len = 1;
            }

            /* Never split a multibyte codepoint at the line-buffer limit. */
            if (render->line.len + display_len <= LINE_BUF_CAP)
                buf_append(&render->line, display_bytes, display_len);
            for (size_t i = 0; i < display_len; i++)
                push_tail_byte(render, display_bytes[i]);
            if (substituted || sanitized[offset] != ' ')
                render->line_has_non_whitespace = 1;
            offset += codepoint_len;
        }
    }

    if (sanitized != stack_sanitized)
        free(sanitized);
    if (stripped != stack_stripped)
        free(stripped);
    disp_flush(render->disp);
}

struct tail_view {
    char bytes[TAIL_RING_CAPACITY];
    size_t len;
    int elided_lines;
};

static void build_tail_view(const struct tool_render *render, struct tail_view *view)
{
    size_t ring_len = render->tail_full ? TAIL_RING_CAPACITY : render->tail_write_pos;
    size_t oldest_pos = render->tail_full ? render->tail_write_pos : 0;
    char linearized[TAIL_RING_CAPACITY];
    for (size_t i = 0; i < ring_len; i++)
        linearized[i] = render->tail[(oldest_pos + i) % TAIL_RING_CAPACITY];

    /* The ring also contains head bytes; the suppressed display-byte count selects its suffix. */
    size_t suppressed_len =
        render->suppressed_tail_bytes < ring_len ? render->suppressed_tail_bytes : ring_len;
    const char *suppressed_bytes = linearized + ring_len - suppressed_len;

    /* Blank lines do not consume visible tail slots. */
    size_t view_start = suppressed_len;
    if (view_start > 0 && suppressed_bytes[view_start - 1] == '\n')
        view_start--;
    size_t current_line_end = view_start;
    int visible_line_count = 0;
    int tail_line_limit = preview_limits_for_mode(render->mode)->tail_lines;
    while (view_start > 0) {
        view_start--;
        if (suppressed_bytes[view_start] != '\n')
            continue;
        size_t candidate_line_start = view_start + 1;
        if (current_line_end > candidate_line_start &&
            !line_is_blank(suppressed_bytes + candidate_line_start,
                           current_line_end - candidate_line_start) &&
            ++visible_line_count == tail_line_limit) {
            view_start = candidate_line_start;
            break;
        }
        current_line_end = view_start;
    }

    /* A wrapped byte ring can begin in the middle of a UTF-8 codepoint. */
    while (view_start < suppressed_len &&
           ((unsigned char)suppressed_bytes[view_start] & 0xC0) == 0x80)
        view_start++;

    size_t view_len = suppressed_len - view_start;
    int elided_line_count = render->suppressed_lines;
    size_t line_start = view_start;
    for (size_t i = view_start; i < suppressed_len; i++) {
        if (suppressed_bytes[i] != '\n')
            continue;
        if (i > line_start && !line_is_blank(suppressed_bytes + line_start, i - line_start))
            elided_line_count--;
        line_start = i + 1;
    }
    if (line_start < suppressed_len &&
        !line_is_blank(suppressed_bytes + line_start, suppressed_len - line_start))
        elided_line_count--;

    memcpy(view->bytes, suppressed_bytes + view_start, view_len);
    view->len = view_len;
    view->elided_lines = elided_line_count > 0 ? elided_line_count : 0;
}

/* The head/tail preview shape shared by the settled rendering and the live spinner view: the
 * elision marker (empty when nothing is elided) and the newest non-blank tail lines, truncated
 * for display. */
struct tail_preview {
    char marker[96];
    char *rows[TAIL_PREVIEW_LINES];
    int row_count;
};

/* newest_bytes (from the line buffer) replaces the view's last line, which the byte-bounded tail
 * ring may have clipped to a long line's ending. */
static void build_tail_preview(const struct tool_render *render, const char *newest_bytes,
                               size_t newest_len, struct tail_preview *preview)
{
    struct tail_view view;
    build_tail_view(render, &view);

    preview->marker[0] = '\0';
    if (view.elided_lines > 0)
        snprintf(preview->marker, sizeof(preview->marker), "... (%d more line%s) ...",
                 view.elided_lines, view.elided_lines == 1 ? "" : "s");

    preview->row_count = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= view.len; i++) {
        if (i < view.len && view.bytes[i] != '\n')
            continue;
        if (i > line_start && !line_is_blank(view.bytes + line_start, i - line_start) &&
            preview->row_count < TAIL_PREVIEW_LINES)
            preview->rows[preview->row_count++] =
                truncate_slice(view.bytes + line_start, i - line_start);
        line_start = i + 1;
    }

    if (newest_bytes && preview->row_count > 0) {
        free(preview->rows[preview->row_count - 1]);
        preview->rows[preview->row_count - 1] = truncate_slice(newest_bytes, newest_len);
    }
}

static void tail_preview_free(struct tail_preview *preview)
{
    for (int row = 0; row < preview->row_count; row++)
        free(preview->rows[row]);
}

/* Live preview repainted in place by the spinner while output streams. */
static void paint_live_tail(struct tool_render *render, const char *bytes, size_t len)
{
    struct tail_preview preview;
    build_tail_preview(render, bytes, len, &preview);

    if (preview.row_count > 0) {
        const char *contents[1 + TAIL_PREVIEW_LINES];
        int count = 0;
        if (preview.marker[0])
            contents[count++] = preview.marker;
        for (int row = 0; row < preview.row_count; row++)
            contents[count++] = preview.rows[row];
        paint_live_rows(render, contents, count);
        render->status_placeholder = 0;
        render->block_open = 1;
    }
    tail_preview_free(&preview);
}

static void finalize_head_tail(struct tool_render *render)
{
    /* The settled tail must show the same newest line the live view showed, not the ring's
     * clipped ending of a long line. */
    const char *newest = render->suppressed_lines > 0 ? render->status_line.data : NULL;
    size_t newest_len = newest ? render->status_line.len : 0;

    struct tail_preview preview;
    build_tail_preview(render, newest, newest_len, &preview);
    if (preview.marker[0])
        replace_status_with_marker(render, preview.marker);
    else
        drop_status(render);
    for (int row = 0; row < preview.row_count; row++)
        emit_row(render, preview.rows[row], strlen(preview.rows[row]));
    tail_preview_free(&preview);
}

static void finalize_capped_preview(struct tool_render *render)
{
    if (!render->head_complete) {
        commit_status(render);
        return;
    }
    if (render->mode == TOOL_RENDER_HEAD_TAIL) {
        finalize_head_tail(render);
        return;
    }
    if (render->suppressed_lines > 0) {
        char marker[96];
        snprintf(marker, sizeof(marker), "... (%d more line%s)", render->suppressed_lines,
                 render->suppressed_lines == 1 ? "" : "s");
        replace_status_with_marker(render, marker);
    }
}

static void flush_pending_input(struct tool_render *render)
{
    char sanitized_tail[UTF8_SANITIZE_FLUSH_MAX];
    size_t sanitized_tail_len = utf8_sanitizer_flush(&render->sanitizer, sanitized_tail);
    if (sanitized_tail_len > 0)
        tool_render_feed(render, sanitized_tail, sanitized_tail_len);

    if (render->mode == TOOL_RENDER_DIFF) {
        if (render->diff_line.len > 0) {
            emit_diff_line(render, render->diff_line.data, render->diff_line.len);
            buf_reset(&render->diff_line);
        }
    } else if (render->line.len > 0) {
        process_line(render, render->line.data, render->line.len, !render->line_has_non_whitespace);
        reset_line(render);
    }
}

void tool_render_finalize(struct tool_render *render)
{
    flush_pending_input(render);
    if (!render->block_open) {
        spinner_hide(render->spinner);
        return;
    }

    /* The swap begins atomically with the spinner's erase; its bracket closes only after the
     * settled rows and the gutter overprint. */
    if (render->mode == TOOL_RENDER_DIFF)
        spinner_swap_begin(render->spinner);
    else
        finalize_capped_preview(render);

    if (render->rows_emitted >= 2)
        overprint_final_gutter(render->disp, GUTTER_LAST);
    else if (render->rows_emitted == 1)
        overprint_final_gutter(render->disp, GUTTER_MARKER);
    spinner_swap_end(render->spinner);
    disp_flush(render->disp);
    render->block_open = 0;
}

void tool_render_emit(const char *bytes, size_t len, void *data)
{
    struct tool_render *render = data;
    render->display_was_called = 1;
    tool_render_feed(render, bytes, len);
}
