/* SPDX-License-Identifier: MIT */
#include "terminal/picker.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "buf.h"
#include "util.h"
#include "terminal/ansi.h"
#include "terminal/input_core.h"
#include "terminal/picker_core.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "text/utf8.h"
#include "text/width.h"

/* Terminal key sequences arrive as a burst; a lone Escape means cancel. */
#define ESC_TIMEOUT_MS 50

#define PICKER_MAX_ROWS 12
/* Search, list separator, and one row below the frame. */
#define PICKER_BASE_ROWS 3

#define PICKER_TITLE_LINES  3
#define PICKER_FOOTER_LINES 4
/* Title/footer separators, search row, and list separator. */
#define PICKER_FRAME_FIXED_ROWS 4
#define PICKER_FRAME_ROWS_MAX                                                                      \
    (PICKER_TITLE_LINES + PICKER_MAX_ROWS + PICKER_FOOTER_LINES + PICKER_FRAME_FIXED_ROWS)

struct picker {
    struct picker_core core;
    int title_lines;
    int footer_lines;
    int painted;
    int previous_row_count;
    /* A terminal resize can reflow each previously painted row. */
    int previous_row_widths[PICKER_FRAME_ROWS_MAX];
    int terminal_cols;
    int terminal_rows;
    struct termios saved_termios;
    int raw_mode_active;
};

static void get_terminal_size(int *terminal_cols, int *terminal_rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *terminal_cols = ws.ws_col > 0 ? ws.ws_col : 80;
        *terminal_rows = ws.ws_row > 0 ? ws.ws_row : 24;
    } else {
        *terminal_cols = 80;
        *terminal_rows = 24;
    }
}

/* Picker content follows the configured display width but can never exceed the
 * physical row its repaint bookkeeping relies on. */
static int picker_width(int terminal_cols)
{
    int width = display_width();
    if (width > terminal_cols)
        width = terminal_cols;
    return width < 1 ? 1 : width;
}

static int enable_raw_mode(struct picker *picker)
{
    if (tcgetattr(STDIN_FILENO, &picker->saved_termios) < 0)
        return -1;
    struct termios raw = picker->saved_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP | BRKINT);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
        return -1;
    picker->raw_mode_active = 1;
    return 0;
}

static void disable_raw_mode(struct picker *picker)
{
    if (!picker->raw_mode_active)
        return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &picker->saved_termios);
    picker->raw_mode_active = 0;
}

static int read_byte_blocking(unsigned char *output)
{
    for (;;) {
        ssize_t bytes_read = read(STDIN_FILENO, output, 1);
        if (bytes_read == 1)
            return 1;
        if (bytes_read == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static int read_byte_timeout(unsigned char *output, int timeout_ms)
{
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
    int result;
    do {
        result = poll(&input, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result <= 0)
        return result;
    return read_byte_blocking(output);
}

/* Replay the byte used to distinguish bare Escape before continuing the timed read. */
struct escape_reader {
    int pending_byte; /* -1 after the first read */
};

static int read_escape_byte(void *user)
{
    struct escape_reader *reader = user;
    if (reader->pending_byte >= 0) {
        int byte = reader->pending_byte;
        reader->pending_byte = -1;
        return byte;
    }
    unsigned char byte;
    return read_byte_timeout(&byte, ESC_TIMEOUT_MS) <= 0 ? -1 : byte;
}

/* Sanitize user-provided text while clipping it to one physical line. */
static void append_clipped_text(struct buf *output, const char *text, int max_cells, int use_utf8)
{
    if (max_cells < 1)
        return;

    size_t len = strlen(text);
    size_t line_len = strcspn(text, "\r\n");
    int natural_cells = 0;
    for (size_t offset = 0; offset < line_len;) {
        size_t bytes;
        int width = utf8_codepoint_cells(text, line_len, offset, &bytes);
        natural_cells += width < 0 ? 1 : width;
        offset += bytes ? bytes : 1;
    }

    int clipped = line_len < len || natural_cells > max_cells;
    int budget = clipped ? max_cells - 1 : max_cells;
    int cells = 0;
    for (size_t offset = 0; offset < line_len;) {
        size_t bytes;
        int width = utf8_codepoint_cells(text, line_len, offset, &bytes);
        int codepoint_cells = width < 0 ? 1 : width;
        if (cells + codepoint_cells > budget)
            break;
        if (width < 0)
            buf_append(output, "?", 1);
        else
            buf_append(output, text + offset, bytes ? bytes : 1);
        cells += codepoint_cells;
        offset += bytes ? bytes : 1;
    }
    if (clipped)
        buf_append_str(output, use_utf8 ? "\xe2\x80\xa6" : ".");
}

static int wrapped_line_count(const char *text, int width, int max_lines)
{
    if (!text || !text[0])
        return 0;

    int lines = 0;
    while (*text && lines < max_lines) {
        size_t separator_bytes;
        size_t line_bytes = wrap_row_bytes(text, (size_t)width, &separator_bytes);
        text += line_bytes + separator_bytes;
        lines++;
    }
    return lines;
}

/* Footer text width after its indent and right margin. Clamped to the content
 * display width so the description prose wraps at the same column as the rest
 * of the app rather than stretching across a wide terminal. */
static int footer_text_width(int terminal_cols)
{
    int w = picker_width(terminal_cols) - PICKER_MARKER_CELLS - 1;
    return w < 8 ? 8 : w;
}

/* Trusted picker ANSI sequences have zero width. Final clipping keeps each logical row on one
 * physical row; the returned width supports cursor recovery after terminal reflow. */
static int append_clipped_line(struct buf *output, const char *line, size_t line_len,
                               int terminal_cols, int use_utf8)
{
    int cells = 0;
    size_t offset = 0;
    while (offset < line_len) {
        if ((unsigned char)line[offset] == 0x1b) {
            size_t escape_end = offset + 1;
            if (escape_end < line_len && line[escape_end] == '[') {
                escape_end++;
                while (escape_end < line_len &&
                       !(line[escape_end] >= 0x40 && line[escape_end] <= 0x7e))
                    escape_end++;
                if (escape_end < line_len)
                    escape_end++;
            }
            buf_append(output, line + offset, escape_end - offset);
            offset = escape_end;
            continue;
        }

        size_t bytes;
        int width = utf8_codepoint_cells(line, line_len, offset, &bytes);
        int codepoint_cells = width < 0 ? 1 : width;
        if (cells + codepoint_cells > terminal_cols) {
            if (cells < terminal_cols) {
                buf_append_str(output, use_utf8 ? "\xe2\x80\xa6" : ".");
                cells++;
            }
            buf_append_str(output, ANSI_RESET);
            return cells;
        }
        if (width < 0)
            buf_append(output, "?", 1);
        else
            buf_append(output, line + offset, bytes ? bytes : 1);
        cells += codepoint_cells;
        offset += bytes ? bytes : 1;
    }
    return cells;
}

static void render_search(struct buf *output, const struct picker *picker, int terminal_cols,
                          int use_utf8)
{
    const char *icon = use_utf8 ? "\xe2\x8c\x95 " : "/ "; /* ⌕ */
    int text_cells = terminal_cols - PICKER_MARKER_CELLS;
    if (text_cells < 0)
        text_cells = 0;

    buf_append_str(output, ANSI_DIM);
    buf_append_str(output, icon);

    if (picker->core.query.len == 0) {
        char placeholder[48];
        snprintf(placeholder, sizeof placeholder, "type to search %zu item%s",
                 picker->core.options->item_count,
                 picker->core.options->item_count == 1 ? "" : "s");
        append_clipped_text(output, placeholder, text_cells, use_utf8);
        buf_append_str(output, ANSI_BOLD_OFF);
        return;
    }

    char count[32];
    snprintf(count, sizeof count, "%zu/%zu", picker->core.match_count,
             picker->core.options->item_count);
    int count_cells = (int)strlen(count);

    int show_count = text_cells >= count_cells + 3;
    int query_cells = show_count ? text_cells - count_cells - 2 : text_cells;

    buf_append_str(output, ANSI_BOLD_OFF); /* query in normal intensity */
    append_clipped_text(output, picker->core.query.data, query_cells, use_utf8);

    if (show_count) {
        buf_append_str(output, "  ");
        buf_append_str(output, ANSI_DIM);
        buf_append_str(output, count);
        buf_append_str(output, ANSI_BOLD_OFF);
    }
}

static void render_row(struct buf *output, const struct picker *picker, size_t match, int selected,
                       int terminal_cols, int use_utf8, int *label_clipped)
{
    const struct picker_item *item = &picker->core.options->items[picker->core.matches[match]];
    int row_cells = terminal_cols - PICKER_MARKER_CELLS;
    if (row_cells < 1)
        row_cells = 1;

    if (selected)
        buf_append_str(output, theme_open(THEME_ACCENT));
    buf_append_str(output, selected ? (use_utf8 ? "\xe2\x86\x92 " : "> ") : "  "); /* → */
    if (selected)
        buf_append_str(output, theme_close(THEME_ACCENT));

    const char *current_tag =
        item->current ? (use_utf8 ? "\xe2\x9c\x93 current" : "* current") : NULL; /* ✓ */
    int current_tag_cells = current_tag ? PICKER_CURRENT_TAG_CELLS : 0;

    const char *separator = item->dim ? (use_utf8 ? " \xe2\x80\x93 " : " - ") : "  "; /* – */
    int separator_cells =
        item->dim ? PICKER_DIM_DETAIL_SEPARATOR_CELLS : PICKER_DETAIL_SEPARATOR_CELLS;

    const char *label = item->label ? item->label : "";
    int label_cells = picker_core_label_cells(item, terminal_cols);

    struct buf detail;
    buf_init(&detail);
    if (item->detail && item->detail[0]) {
        int detail_cells = row_cells - current_tag_cells - label_cells - separator_cells;
        append_clipped_text(&detail, item->detail, detail_cells, use_utf8);
    }

    if (label_clipped)
        *label_clipped = picker_core_text_cells(label) > label_cells;
    /* The arrow remains the focus indicator when a selected item is dim. */
    if (item->dim)
        buf_append_str(output, ANSI_DIM);
    else if (selected)
        buf_append_str(output, ANSI_BOLD);
    /* A custom foreground is label content; selection does not replace it. */
    int color_label = !item->dim && item->label_color && item->label_color[0];
    if (color_label)
        buf_append_str(output, item->label_color);
    append_clipped_text(output, label, label_cells, use_utf8);
    if (color_label)
        buf_append_str(output, ANSI_FG_DEFAULT);
    if (item->dim || selected)
        buf_append_str(output, ANSI_BOLD_OFF);

    if (current_tag) {
        buf_append_str(output, "  ");
        buf_append_str(output, theme_open(THEME_OK));
        buf_append_str(output, current_tag);
        buf_append_str(output, theme_close(THEME_OK));
    }
    if (detail.len) {
        buf_append_str(output, ANSI_DIM);
        buf_append_str(output, separator);
        buf_append(output, detail.data ? detail.data : "", detail.len);
        buf_append_str(output, ANSI_BOLD_OFF);
    }
    buf_free(&detail);
}

struct frame {
    struct buf output;
    struct buf row;
    int row_count;
    int row_widths[PICKER_FRAME_ROWS_MAX];
    int width;
    int use_utf8;
};

static void frame_init(struct frame *frame, int terminal_cols, int use_utf8)
{
    buf_init(&frame->output);
    buf_init(&frame->row);
    frame->row_count = 0;
    memset(frame->row_widths, 0, sizeof(frame->row_widths));
    frame->width = terminal_cols;
    frame->use_utf8 = use_utf8;
}

/* Emit one clipped physical line and reset the row buffer. */
static void frame_emit(struct frame *frame)
{
    if (frame->row_count)
        buf_append_str(&frame->output, "\r\n");
    int cells = append_clipped_line(&frame->output, frame->row.data ? frame->row.data : "",
                                    frame->row.len, frame->width, frame->use_utf8);
    if (frame->row_count < PICKER_FRAME_ROWS_MAX)
        frame->row_widths[frame->row_count] = cells;
    frame->row_count++;
    buf_append_str(&frame->output, ANSI_ERASE_LINE);
    buf_reset(&frame->row);
}

static void frame_free(struct frame *frame)
{
    buf_free(&frame->output);
    buf_free(&frame->row);
}

static void render_title(struct frame *frame, const char *title, int line_count)
{
    const char *remaining = title;
    for (int line = 0; line < line_count; line++) {
        buf_append_str(&frame->row, ANSI_BOLD);
        if (line == line_count - 1) {
            append_clipped_text(&frame->row, remaining, frame->width, frame->use_utf8);
        } else {
            size_t separator_bytes;
            size_t line_bytes = wrap_row_bytes(remaining, (size_t)frame->width, &separator_bytes);
            picker_core_append_sanitized(&frame->row, remaining, line_bytes);
            remaining += line_bytes + separator_bytes;
        }
        buf_append_str(&frame->row, ANSI_BOLD_OFF);
        frame_emit(frame);
    }
    frame_emit(frame); /* blank line between the title and the search field */
}

/* A fixed footer height prevents the list from moving as selection changes. */
static void render_footer(struct frame *frame, const struct picker *picker,
                          int selected_label_clipped)
{
    if (picker->footer_lines <= 0)
        return;
    frame_emit(frame);

    const struct picker_item *selected_item =
        picker->core.match_count
            ? &picker->core.options->items[picker->core.matches[picker->core.selection]]
            : NULL;
    const char *description = selected_item ? selected_item->description : NULL;
    char *combined_description = NULL;
    if (selected_item && selected_item->label && picker->core.options->repeat_clipped_label &&
        selected_label_clipped) {
        if (description && description[0])
            description = combined_description =
                xasprintf("%s\n%s", description, selected_item->label);
        else
            description = selected_item->label;
    }

    int text_cells = footer_text_width(frame->width);
    const char *remaining = description && description[0] ? description : "";
    for (int line = 0; line < picker->footer_lines; line++) {
        if (*remaining) {
            buf_append_str(&frame->row, ANSI_DIM "  ");
            if (line == picker->footer_lines - 1) {
                append_clipped_text(&frame->row, remaining, text_cells, frame->use_utf8);
            } else {
                size_t separator_bytes;
                size_t line_bytes = wrap_row_bytes(remaining, (size_t)text_cells, &separator_bytes);
                picker_core_append_sanitized(&frame->row, remaining, line_bytes);
                remaining += line_bytes + separator_bytes;
            }
            buf_append_str(&frame->row, ANSI_BOLD_OFF);
        }
        frame_emit(frame);
    }
    free(combined_description);
}

/* Viewport and footer reservations both depend on terminal geometry. */
static void picker_layout(struct picker *picker, int terminal_cols, int terminal_rows)
{
    const struct picker_opts *options = picker->core.options;
    picker->terminal_cols = terminal_cols;
    picker->terminal_rows = terminal_rows;

    int width = picker_width(terminal_cols);
    picker->title_lines =
        options->title ? wrapped_line_count(options->title, width, PICKER_TITLE_LINES) : 0;

    int footer_cells = footer_text_width(width);
    picker->footer_lines = 0;
    for (size_t i = 0; i < options->item_count; i++) {
        const struct picker_item *item = &options->items[i];
        int item_lines = wrapped_line_count(item->description, footer_cells, PICKER_FOOTER_LINES);
        /* Detail can make a label clip before it reaches the full row width. */
        if (options->repeat_clipped_label && item->label &&
            picker_core_text_cells(item->label) > picker_core_label_cells(item, width)) {
            item_lines +=
                wrapped_line_count(item->label, footer_cells, PICKER_FOOTER_LINES - item_lines);
        }
        if (item_lines > picker->footer_lines)
            picker->footer_lines = item_lines;
    }

    int reserved_rows = PICKER_BASE_ROWS;
    if (picker->title_lines)
        reserved_rows += picker->title_lines + 1;
    if (picker->footer_lines > 0)
        reserved_rows += picker->footer_lines + 1;
    int viewport_rows = terminal_rows - reserved_rows;
    if (viewport_rows < 1)
        viewport_rows = 1;
    if (viewport_rows > PICKER_MAX_ROWS)
        viewport_rows = PICKER_MAX_ROWS;
    picker->core.viewport_rows = viewport_rows;
}

/* A width change can reflow each old logical row across multiple physical rows. This calculation
 * assumes xterm-style reflow; terminals that truncate on resize may leave stale content. */
static int reflow_climb(const struct picker *picker, int terminal_cols, int terminal_rows)
{
    if (!picker->painted || picker->previous_row_count <= 0 || terminal_cols <= 0)
        return 0;

    int recorded_rows = picker->previous_row_count < PICKER_FRAME_ROWS_MAX
                            ? picker->previous_row_count
                            : PICKER_FRAME_ROWS_MAX;
    int climb = reflow_physical_rows(picker->previous_row_widths, recorded_rows, terminal_cols) - 1;
    /* Climbing beyond the screen top would start the repaint on the wrong row. */
    if (terminal_rows > 0 && climb > terminal_rows - 1)
        climb = terminal_rows - 1;
    return climb < 0 ? 0 : climb;
}

static void paint(struct picker *picker)
{
    int terminal_cols, terminal_rows;
    get_terminal_size(&terminal_cols, &terminal_rows);
    if (terminal_cols != picker->terminal_cols || terminal_rows != picker->terminal_rows) {
        picker_layout(picker, terminal_cols, terminal_rows);
        picker_core_clamp_view(&picker->core);
    }

    struct frame frame;
    frame_init(&frame, picker_width(terminal_cols), locale_have_utf8());

    /* Redraw before erasing stale tails for terminals that ignore synchronized output. */
    buf_append_str(&frame.output, ANSI_SYNC_BEGIN);

    int climb = reflow_climb(picker, terminal_cols, terminal_rows);
    if (climb > 0) {
        char cursor_up[16];
        snprintf(cursor_up, sizeof cursor_up, ANSI_CSI "%dA", climb);
        buf_append_str(&frame.output, cursor_up);
    }
    if (picker->painted)
        buf_append(&frame.output, "\r", 1);

    if (picker->title_lines)
        render_title(&frame, picker->core.options->title, picker->title_lines);

    render_search(&frame.row, picker, frame.width, frame.use_utf8);
    frame_emit(&frame);

    frame_emit(&frame); /* blank line between the search field and the list */

    int selected_label_clipped = 0;
    if (picker->core.match_count == 0) {
        buf_append_str(&frame.row, ANSI_DIM "  (no matches)" ANSI_BOLD_OFF);
        frame_emit(&frame);
    } else {
        size_t first_hidden = picker->core.first_visible + (size_t)picker->core.viewport_rows;
        if (first_hidden > picker->core.match_count)
            first_hidden = picker->core.match_count;
        for (size_t match = picker->core.first_visible; match < first_hidden; match++) {
            int row_selected = match == picker->core.selection;
            render_row(&frame.row, picker, match, row_selected, frame.width, frame.use_utf8,
                       row_selected ? &selected_label_clipped : NULL);
            frame_emit(&frame);
        }
    }

    render_footer(&frame, picker, selected_label_clipped);

    buf_append_str(&frame.output, ANSI_ERASE_BELOW);
    buf_append_str(&frame.output, ANSI_SYNC_END);

    fwrite(frame.output.data ? frame.output.data : "", 1, frame.output.len, stdout);
    fflush(stdout);

    memcpy(picker->previous_row_widths, frame.row_widths, sizeof(picker->previous_row_widths));
    picker->previous_row_count = frame.row_count;
    picker->painted = 1;
    frame_free(&frame);
}

enum picker_input_result {
    PICKER_INPUT_CONTINUE,
    PICKER_INPUT_ACCEPT,
    PICKER_INPUT_CANCEL,
};

static void apply_navigation_action(struct picker_core *core, enum input_action action)
{
    switch (action) {
    case INPUT_ACTION_HISTORY_PREV:
        picker_core_move_selection(core, PICKER_DIRECTION_PREVIOUS);
        break;
    case INPUT_ACTION_HISTORY_NEXT:
        picker_core_move_selection(core, PICKER_DIRECTION_NEXT);
        break;
    case INPUT_ACTION_LINE_START:
        picker_core_select_first(core);
        break;
    case INPUT_ACTION_LINE_END:
        picker_core_select_last(core);
        break;
    case INPUT_ACTION_PAGE_UP:
        picker_core_page_selection(core, PICKER_DIRECTION_PREVIOUS);
        break;
    case INPUT_ACTION_PAGE_DOWN:
        picker_core_page_selection(core, PICKER_DIRECTION_NEXT);
        break;
    default:
        break;
    }
}

static enum picker_input_result process_input_byte(struct picker_core *core, unsigned char byte)
{
    if (byte == 0x03 || byte == 0x07) /* Ctrl-C / Ctrl-G */
        return PICKER_INPUT_CANCEL;
    if (byte == 0x0d || byte == 0x0a) /* Enter / LF */
        return core->match_count ? PICKER_INPUT_ACCEPT : PICKER_INPUT_CONTINUE;
    if (byte == 0x7f || byte == 0x08) { /* Backspace */
        if (core->query.len) {
            core->query.len = utf8_prev(core->query.data, core->query.len);
            core->query.data[core->query.len] = '\0';
            picker_core_update_matches(core);
        }
        return PICKER_INPUT_CONTINUE;
    }
    if (byte == 0x15) { /* Ctrl-U */
        if (core->query.len) {
            core->query.len = 0;
            core->query.data[0] = '\0';
            picker_core_update_matches(core);
        }
        return PICKER_INPUT_CONTINUE;
    }
    if (byte == 0x0e || byte == 0x10) { /* Ctrl-N / Ctrl-P */
        picker_core_move_selection(core, byte == 0x0e ? PICKER_DIRECTION_NEXT
                                                      : PICKER_DIRECTION_PREVIOUS);
        return PICKER_INPUT_CONTINUE;
    }
    if (byte == 0x1b) {
        unsigned char next_byte;
        if (read_byte_timeout(&next_byte, ESC_TIMEOUT_MS) <= 0)
            return PICKER_INPUT_CANCEL;
        struct escape_reader reader = {.pending_byte = next_byte};
        enum input_action action = input_core_decode_escape(read_escape_byte, &reader);
        apply_navigation_action(core, action);
        return PICKER_INPUT_CONTINUE;
    }
    if (byte < 0x20)
        return PICKER_INPUT_CONTINUE;

    size_t sequence_len = utf8_sequence_length(byte);
    char bytes[4] = {(char)byte};
    size_t bytes_read = 1;
    for (size_t i = 1; i < sequence_len; i++) {
        unsigned char continuation;
        if (read_byte_timeout(&continuation, ESC_TIMEOUT_MS) <= 0)
            break;
        bytes[bytes_read++] = (char)continuation;
    }
    buf_append(&core->query, bytes, bytes_read);
    picker_core_update_matches(core);
    return PICKER_INPUT_CONTINUE;
}

long picker_run(const struct picker_opts *options)
{
    if (!options || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return -1;
    if (options->item_count == 0) {
        if (options->empty_message)
            ui_note("%s", options->empty_message);
        return -1;
    }
    if (!options->items)
        return -1;

    struct picker picker;
    memset(&picker, 0, sizeof picker);
    picker.core.options = options;
    picker.core.matches = xmalloc(options->item_count * sizeof(*picker.core.matches));
    buf_init(&picker.core.query);

    int terminal_cols, terminal_rows;
    get_terminal_size(&terminal_cols, &terminal_rows);
    picker_layout(&picker, terminal_cols, terminal_rows);

    picker_core_update_matches(&picker.core);
    if (options->initial_index)
        picker_core_select_item(&picker.core, options->initial_index);

    if (enable_raw_mode(&picker) < 0) {
        free(picker.core.matches);
        buf_free(&picker.core.query);
        return -1;
    }
    /* Selection, not the search field, is the focus indicator. */
    fputs(ANSI_CURSOR_HIDE, stdout);
    fflush(stdout);
    paint(&picker);

    long result = -1;
    for (;;) {
        unsigned char byte;
        if (read_byte_blocking(&byte) <= 0)
            break;

        enum picker_input_result input_result = process_input_byte(&picker.core, byte);
        if (input_result == PICKER_INPUT_CANCEL)
            break;
        if (input_result == PICKER_INPUT_ACCEPT) {
            result = (long)picker.core.matches[picker.core.selection];
            break;
        }
        paint(&picker);
    }

    /* Erase the picker's painted area; leave the cursor at column 0 of a
     * clean line for the caller's next output, and restore the cursor. */
    if (picker.painted && picker.previous_row_count > 0) {
        int current_terminal_cols, current_terminal_rows;
        get_terminal_size(&current_terminal_cols, &current_terminal_rows);
        int climb = reflow_climb(&picker, current_terminal_cols, current_terminal_rows);
        fputs(ANSI_SYNC_BEGIN, stdout);
        if (climb > 0)
            printf(ANSI_CSI "%dA", climb);
        fputs("\r" ANSI_ERASE_BELOW ANSI_SYNC_END, stdout);
    }
    fputs(ANSI_CURSOR_SHOW, stdout);
    fflush(stdout);
    disable_raw_mode(&picker);

    free(picker.core.matches);
    buf_free(&picker.core.query);
    return result;
}
