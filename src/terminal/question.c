/* SPDX-License-Identifier: MIT */
#include "terminal/question.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "buf.h"
#include "xalloc.h"
#include "system/locale.h"
#include "terminal/ansi.h"
#include "terminal/input_core.h"
#include "terminal/theme.h"
#include "terminal/width.h"
#include "text/utf8.h"
#include "text/width.h"

/* Terminal key sequences arrive as a burst; a lone Escape means cancel. */
#define ESC_TIMEOUT_MS 50

#define QUESTION_MAX            5
#define OPTION_INDENT_CELLS     3
#define OPTION_MARKER_CELLS     5 /* "  1. " */
#define FREE_TEXT_MARK          "Type something."
#define FREE_TEXT_MARK_FALLBACK "Type something"
#define FRAME_ROWS_MAX          64

struct question_answer {
    int has_answer;
    int index; /* 1-based option index, -1 for free text */
    char *text;
    char *value;
    int was_custom;
};

struct question {
    struct termios saved_termios;
    int raw_mode_active;
    int painted;
    int previous_row_count;
    int previous_row_widths[FRAME_ROWS_MAX];

    size_t question_count;
    size_t tab; /* question index, or question_count for the Submit tab */
    int input_mode;
    int empty_hint;
    size_t option_index;

    struct buf text; /* free-text input buffer, NUL-terminated */
    struct question_answer answers[QUESTION_MAX];
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

static int question_width(int terminal_cols)
{
    int width = display_width();
    if (width > terminal_cols)
        width = terminal_cols;
    return width < 1 ? 1 : width;
}

static int enable_raw_mode(struct question *question)
{
    if (tcgetattr(STDIN_FILENO, &question->saved_termios) < 0)
        return -1;
    struct termios raw = question->saved_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP | BRKINT);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
        return -1;
    question->raw_mode_active = 1;
    return 0;
}

static void disable_raw_mode(struct question *question)
{
    if (!question->raw_mode_active)
        return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &question->saved_termios);
    question->raw_mode_active = 0;
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

/* Replay the byte that distinguishes bare Escape before continuing the timed read, and record
 * consumed bytes so Shift+Tab (CSI Z), which the shared decoder reports as unknown, is visible. */
struct escape_reader {
    int pending_byte; /* -1 after the first read */
    char sequence[8];
    size_t sequence_len;
};

static int read_escape_byte(void *user)
{
    struct escape_reader *reader = user;
    int byte;
    if (reader->pending_byte >= 0) {
        byte = reader->pending_byte;
        reader->pending_byte = -1;
    } else {
        unsigned char raw;
        byte = read_byte_timeout(&raw, ESC_TIMEOUT_MS) <= 0 ? -1 : raw;
    }
    if (byte >= 0 && reader->sequence_len < sizeof reader->sequence)
        reader->sequence[reader->sequence_len++] = (char)byte;
    return byte;
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

/* Trusted question ANSI sequences have zero width. Final clipping keeps each logical row on one
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

struct frame {
    struct buf output;
    struct buf row;
    int row_count;
    int row_widths[FRAME_ROWS_MAX];
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
    if (frame->row_count < FRAME_ROWS_MAX)
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

/* Selectable rows include the implicit free-text option. */
static size_t option_count(const struct ask_question *ask)
{
    return ask->n_options + (ask->allow_other ? 1 : 0);
}

static int is_free_text_option(const struct ask_question *ask, size_t option_index)
{
    return ask->allow_other && option_index == ask->n_options;
}

static void question_select(struct question *question, const struct ask_question *ask,
                            size_t option_index)
{
    struct question_answer *answer = &question->answers[question->tab];
    free(answer->text);
    free(answer->value);
    answer->text = NULL;
    answer->value = NULL;
    answer->has_answer = 1;
    if (is_free_text_option(ask, option_index)) {
        answer->index = -1;
        answer->text = xstrdup(question->text.data ? question->text.data : "");
        answer->value = xstrdup(answer->text);
        answer->was_custom = 1;
    } else {
        const struct ask_question_option *option = &ask->options[option_index];
        answer->index = (int)option_index + 1;
        answer->text = xstrdup(option->label ? option->label : "");
        answer->value = xstrdup(option->value && option->value[0] ? option->value
                                : option->label                   ? option->label
                                                                  : "");
        answer->was_custom = 0;
    }
    buf_reset(&question->text);
}

/* Revisiting a question with an answer pre-selects it, or its free-text option. A NULL ask (the
 * Submit tab) only resets the editing state, since there is no option list to index. */
static void question_reselect(struct question *question, const struct ask_question *ask)
{
    question->input_mode = 0;
    question->empty_hint = 0;
    question->option_index = 0;
    if (!ask)
        return;

    const struct question_answer *answer = &question->answers[question->tab];
    if (!answer->has_answer)
        return;
    if (answer->index < 0) {
        if (ask->allow_other)
            question->option_index = ask->n_options;
    } else if (answer->index > 0 && (size_t)(answer->index - 1) < ask->n_options) {
        question->option_index = (size_t)(answer->index - 1);
    } else if (ask->allow_other) {
        question->option_index = ask->n_options;
    }
}

static void question_move_option(struct question *question, const struct ask_question *ask,
                                 int direction)
{
    if (!ask)
        return;
    size_t count = option_count(ask);
    if (count == 0)
        return;
    if (question->option_index >= count)
        question->option_index = count - 1;
    if (direction < 0) {
        if (question->option_index > 0)
            question->option_index--;
    } else if (question->option_index + 1 < count) {
        question->option_index++;
    }
}

/* Wrap through the question tabs and the trailing Submit tab, pre-selecting any saved answer. */
static void question_move_tab(struct question *question, const struct ask_question *questions,
                              int direction)
{
    size_t count = question->question_count;
    if (count <= 1)
        return;
    size_t total = count + 1;
    if (direction < 0)
        question->tab = (question->tab + total - 1) % total;
    else
        question->tab = (question->tab + 1) % total;
    question_reselect(question, question->tab < count ? &questions[question->tab] : NULL);
}

/* Record then move on: a single question completes, otherwise the next question or the Submit tab.
 * Returns 1 when the widget is finished. */
static int question_advance(struct question *question, const struct ask_question *questions)
{
    if (question->question_count <= 1)
        return 1;
    if (question->tab < question->question_count - 1)
        question->tab++;
    else
        question->tab = question->question_count;
    question_reselect(question,
                      question->tab < question->question_count ? &questions[question->tab] : NULL);
    return 0;
}

static int reflow_climb(const struct question *question, int terminal_cols, int terminal_rows);

static void paint(struct question *question, const struct ask_question *questions)
{
    int terminal_cols, terminal_rows;
    get_terminal_size(&terminal_cols, &terminal_rows);

    struct frame frame;
    frame_init(&frame, question_width(terminal_cols), locale_have_utf8());
    int use_utf8 = frame.use_utf8;
    const char *chrome = theme_open(THEME_ACCENT);
    const char *chrome_close = theme_close(THEME_ACCENT);
    char bar[2];
    const char *bar_char = use_utf8 ? "\xe2\x94\x80" : "-";
    snprintf(bar, sizeof bar, "%s%s%s", chrome, bar_char, chrome_close);

    /* Redraw before erasing stale tails for terminals that ignore synchronized output. */
    buf_append_str(&frame.output, ANSI_SYNC_BEGIN);

    int climb = reflow_climb(question, terminal_cols, terminal_rows);
    if (climb > 0) {
        char cursor_up[16];
        snprintf(cursor_up, sizeof cursor_up, ANSI_CSI "%dA", climb);
        buf_append_str(&frame.output, cursor_up);
    }
    if (question->painted)
        buf_append(&frame.output, "\r", 1);

    const struct ask_question *ask =
        question->tab < question->question_count ? &questions[question->tab] : NULL;

    frame_emit(&frame); /* top frame bar */
    for (int i = 0; i < frame.width; i++)
        buf_append_str(&frame.row, bar);
    frame_emit(&frame);

    /* Tab bar: one cell per question plus the Submit cell. */
    if (question->question_count > 1) {
        const char *left = use_utf8 ? "\xe2\x86\x90 " : "< ";
        const char *right = use_utf8 ? " \xe2\x86\x92" : " >";
        buf_append_str(&frame.row, left);
        int submitted = question->tab == question->question_count;
        int all_answered = 1;
        for (size_t i = 0; i < question->question_count; i++)
            if (!question->answers[i].has_answer)
                all_answered = 0;
        for (size_t i = 0; i < question->question_count; i++) {
            const struct ask_question *item = &questions[i];
            int active = question->tab == i;
            int answered = question->answers[i].has_answer;
            if (i)
                buf_append_str(&frame.row, " ");
            if (active)
                buf_append_str(&frame.row, chrome);
            else
                buf_append_str(&frame.row, answered ? theme_open(THEME_OK) : ANSI_DIM);
            buf_append_str(&frame.row, answered ? (use_utf8 ? "\xe2\x96\xa0 " : "* ")
                                                : (use_utf8 ? "\xe2\x96\x91 " : "- "));
            append_clipped_text(&frame.row, item->label ? item->label : "", use_utf8 ? 24 : 22,
                                use_utf8);
            if (active)
                buf_append_str(&frame.row, chrome_close);
            else
                buf_append_str(&frame.row, answered ? theme_close(THEME_OK) : ANSI_BOLD_OFF);
        }
        int active = submitted;
        if (active)
            buf_append_str(&frame.row, chrome);
        else
            buf_append_str(&frame.row, all_answered ? theme_open(THEME_OK) : ANSI_DIM);
        buf_append_str(&frame.row, all_answered ? (use_utf8 ? "\xe2\x9c\x93 " : "v ")
                                                : (use_utf8 ? "\xe2\x9c\x97 " : "x "));
        buf_append_str(&frame.row, "Submit ");
        if (active)
            buf_append_str(&frame.row, chrome_close);
        else
            buf_append_str(&frame.row, all_answered ? theme_close(THEME_OK) : ANSI_BOLD_OFF);
        buf_append_str(&frame.row, right);
        frame_emit(&frame);
        frame_emit(&frame); /* blank line between the tab bar and the content */
    } else {
        frame_emit(&frame); /* blank line between the frame bar and the content */
    }

    if (ask) {
        if (question->question_count > 1) {
            char heading[64];
            snprintf(heading, sizeof heading, " Q%zu of %zu", question->tab + 1,
                     question->question_count);
            buf_append_str(&frame.row, ANSI_BOLD);
            append_clipped_text(&frame.row, heading, 40, use_utf8);
            buf_append_str(&frame.row, ANSI_BOLD_OFF);
        }
        if (ask->label && ask->label[0]) {
            buf_append_str(&frame.row, " ");
            buf_append_str(&frame.row, ANSI_BOLD);
            append_clipped_text(&frame.row, ask->label, 48, use_utf8);
            buf_append_str(&frame.row, ANSI_BOLD_OFF);
        }
        frame_emit(&frame);

        const char *prompt = ask->prompt ? ask->prompt : "";
        const char *remaining = prompt;
        for (;;) {
            size_t separator_bytes;
            size_t line_bytes = wrap_row_bytes(remaining, (size_t)frame.width, &separator_bytes);
            buf_append_str(&frame.row, " ");
            buf_append(&frame.row, remaining, line_bytes);
            frame_emit(&frame);
            remaining += line_bytes + separator_bytes;
            if (!*remaining)
                break;
        }
        frame_emit(&frame); /* blank line between the prompt and the options */

        size_t selectable = option_count(ask);
        for (size_t i = 0; i < selectable; i++) {
            int selected = i == question->option_index;
            if (selected)
                buf_append_str(&frame.row, chrome);
            append_clipped_text(&frame.row, " ", OPTION_INDENT_CELLS, use_utf8);
            char marker[16];
            snprintf(marker, sizeof marker, "%zu. ", i + 1);
            append_clipped_text(&frame.row, marker, 8, use_utf8);
            const char *label = is_free_text_option(ask, i)
                                    ? (use_utf8 ? FREE_TEXT_MARK : FREE_TEXT_MARK_FALLBACK)
                                    : ask->options[i].label;
            if (label)
                append_clipped_text(&frame.row, label, frame.width - 8, use_utf8);
            if (selected)
                buf_append_str(&frame.row, chrome_close);
            frame_emit(&frame);
            if (i < ask->n_options && ask->options[i].description &&
                ask->options[i].description[0]) {
                buf_append_str(&frame.row, "     ");
                buf_append_str(&frame.row, ANSI_DIM);
                append_clipped_text(&frame.row, ask->options[i].description, frame.width - 5,
                                    use_utf8);
                buf_append_str(&frame.row, ANSI_BOLD_OFF);
                frame_emit(&frame);
            }
        }
        frame_emit(&frame);

        if (question->input_mode) {
            buf_append_str(&frame.row, " ");
            buf_append_str(&frame.row, theme_open(THEME_ACCENT));
            append_clipped_text(&frame.row, question->text.data ? question->text.data : "",
                                frame.width - 2, use_utf8);
            buf_append_str(&frame.row, chrome_close);
            frame_emit(&frame);
            frame_emit(&frame);
            buf_append_str(&frame.row, " ");
            if (question->empty_hint) {
                buf_append_str(&frame.row, theme_open(THEME_WARN));
                buf_append_str(&frame.row, "Enter an answer first");
                buf_append_str(&frame.row, theme_close(THEME_WARN));
            } else {
                buf_append_str(&frame.row, ANSI_DIM);
                buf_append_str(&frame.row, use_utf8 ? "Enter answer \xe2\x80\xa2 Esc back"
                                                    : "Enter answer . Esc back");
                buf_append_str(&frame.row, ANSI_BOLD_OFF);
            }
        } else {
            buf_append_str(&frame.row, " ");
            buf_append_str(&frame.row, ANSI_DIM);
            buf_append_str(&frame.row,
                           question->question_count > 1
                               ? (use_utf8 ? "up/down move \xe2\x80\xa2 Tab next \xe2\x80\xa2 "
                                             "Enter select \xe2\x80\xa2 Esc cancel"
                                           : "up/down move . Tab next . Enter select . Esc cancel")
                               : (use_utf8 ? "up/down move \xe2\x80\xa2 Enter select \xe2\x80\xa2 "
                                             "Esc cancel"
                                           : "up/down move . Enter select . Esc cancel"));
            buf_append_str(&frame.row, ANSI_BOLD_OFF);
        }
        frame_emit(&frame);
    } else {
        /* Submit tab: summary of current answers. */
        buf_append_str(&frame.row, ANSI_BOLD);
        append_clipped_text(&frame.row, "Review", 40, use_utf8);
        buf_append_str(&frame.row, ANSI_BOLD_OFF);
        frame_emit(&frame);

        for (size_t i = 0; i < question->question_count; i++) {
            const struct ask_question *item = &questions[i];
            const struct question_answer *item_answer = &question->answers[i];
            buf_append_str(&frame.row, " ");
            buf_append_str(&frame.row, ANSI_DIM);
            buf_append_str(&frame.row, item->label ? item->label : "");
            buf_append_str(&frame.row, ": ");
            buf_append_str(&frame.row, ANSI_BOLD_OFF);
            if (item_answer->has_answer) {
                if (item_answer->was_custom) {
                    buf_append_str(&frame.row, ANSI_DIM);
                    buf_append_str(&frame.row, "(wrote) ");
                    buf_append_str(&frame.row, ANSI_BOLD_OFF);
                }
                append_clipped_text(&frame.row, item_answer->text, frame.width - 20, use_utf8);
            } else {
                buf_append_str(&frame.row, theme_open(THEME_WARN));
                buf_append_str(&frame.row, "unanswered");
                buf_append_str(&frame.row, theme_close(THEME_WARN));
            }
            frame_emit(&frame);
        }
        frame_emit(&frame);

        int all_answered = 1;
        size_t missing = 0;
        for (size_t i = 0; i < question->question_count; i++) {
            if (question->answers[i].has_answer)
                continue;
            all_answered = 0;
            missing++;
        }
        buf_append_str(&frame.row, " ");
        buf_append_str(&frame.row, ANSI_DIM);
        if (all_answered) {
            buf_append_str(&frame.row, "All questions answered");
        } else {
            char missing_text[64];
            snprintf(missing_text, sizeof missing_text, "%zu question%s unanswered", missing,
                     missing == 1 ? "" : "s");
            buf_append_str(&frame.row, missing_text);
        }
        buf_append_str(&frame.row, ANSI_BOLD_OFF);
        frame_emit(&frame);
    }

    /* bottom frame bar */
    for (int i = 0; i < frame.width; i++)
        buf_append_str(&frame.row, bar);
    frame_emit(&frame);

    buf_append_str(&frame.output, ANSI_ERASE_BELOW);
    buf_append_str(&frame.output, ANSI_SYNC_END);

    fwrite(frame.output.data ? frame.output.data : "", 1, frame.output.len, stdout);
    fflush(stdout);

    memcpy(question->previous_row_widths, frame.row_widths, sizeof(frame.row_widths));
    question->previous_row_count = frame.row_count;
    question->painted = 1;
    frame_free(&frame);
}

/* A width change can reflow each old logical row across multiple physical rows. This calculation
 * assumes xterm-style reflow; terminals that truncate on resize may leave stale content. */
static int reflow_climb(const struct question *question, int terminal_cols, int terminal_rows)
{
    if (!question->painted || question->previous_row_count <= 0 || terminal_cols <= 0)
        return 0;

    int recorded_rows = question->previous_row_count < FRAME_ROWS_MAX ? question->previous_row_count
                                                                      : FRAME_ROWS_MAX;
    int climb =
        reflow_physical_rows(question->previous_row_widths, recorded_rows, terminal_cols) - 1;
    /* Climbing beyond the screen top would start the repaint on the wrong row. */
    if (terminal_rows > 0 && climb > terminal_rows - 1)
        climb = terminal_rows - 1;
    return climb < 0 ? 0 : climb;
}

void ask_result_free(struct ask_result *result)
{
    if (!result)
        return;
    for (size_t i = 0; i < result->n_answers; i++) {
        free(result->answers[i].text);
        free(result->answers[i].value);
    }
    free(result->answers);
    memset(result, 0, sizeof *result);
}

int question_run(const struct ask_question *questions, size_t n_questions,
                 struct ask_result *result)
{
    if (!result)
        return -1;
    memset(result, 0, sizeof *result);
    result->cancelled = 1;
    if (!questions || n_questions < 1 || n_questions > QUESTION_MAX || !isatty(STDIN_FILENO) ||
        !isatty(STDOUT_FILENO))
        return -1;

    struct question question;
    memset(&question, 0, sizeof question);
    question.question_count = n_questions;
    buf_init(&question.text);
    question_reselect(&question, &questions[0]);

    if (enable_raw_mode(&question) < 0) {
        buf_free(&question.text);
        return -1;
    }
    fputs(ANSI_CURSOR_HIDE, stdout);
    fflush(stdout);
    paint(&question, questions);

    int cancelled = 0;
    for (;;) {
        unsigned char byte;
        if (read_byte_blocking(&byte) <= 0) {
            cancelled = 1;
            break;
        }

        if (byte == 0x03) { /* Ctrl-C */
            cancelled = 1;
            break;
        }

        if (byte == 0x1b) {
            unsigned char next_byte;
            if (read_byte_timeout(&next_byte, ESC_TIMEOUT_MS) <= 0) {
                /* Bare Esc leaves free-text input and clears it; elsewhere it cancels the ask. */
                if (question.input_mode) {
                    question.input_mode = 0;
                    question.empty_hint = 0;
                    buf_reset(&question.text);
                } else {
                    cancelled = 1;
                    break;
                }
            } else {
                struct escape_reader reader = {.pending_byte = next_byte, .sequence_len = 0};
                enum input_action action = input_core_decode_escape(read_escape_byte, &reader);
                /* Shift+Tab arrives as CSI Z, which the shared decoder reports as unknown. */
                if (action == INPUT_ACTION_NONE && reader.sequence_len >= 2 &&
                    reader.sequence[0] == '[' && reader.sequence[reader.sequence_len - 1] == 'Z')
                    action = INPUT_ACTION_MOVE_LEFT;
                /* Free-text input has no cursor editor, so sequence motion is ignored. */
                if (!question.input_mode) {
                    const struct ask_question *ask =
                        question.tab < question.question_count ? &questions[question.tab] : NULL;
                    switch (action) {
                    case INPUT_ACTION_HISTORY_PREV:
                        question_move_option(&question, ask, -1);
                        break;
                    case INPUT_ACTION_HISTORY_NEXT:
                        question_move_option(&question, ask, 1);
                        break;
                    case INPUT_ACTION_MOVE_LEFT:
                        question_move_tab(&question, questions, -1);
                        break;
                    case INPUT_ACTION_MOVE_RIGHT:
                        question_move_tab(&question, questions, 1);
                        break;
                    default:
                        break;
                    }
                }
            }
        } else if (question.input_mode) {
            if (byte == 0x0d || byte == 0x0a) { /* Enter */
                if (question.text.len > 0) {
                    const struct ask_question *ask = &questions[question.tab];
                    question_select(&question, ask, ask->n_options);
                    if (question_advance(&question, questions))
                        break;
                } else {
                    question.empty_hint = 1;
                }
            } else if (byte == 0x7f || byte == 0x08) { /* Backspace */
                if (question.text.len) {
                    question.text.len = utf8_prev(question.text.data, question.text.len);
                    question.text.data[question.text.len] = '\0';
                }
                question.empty_hint = 0;
            } else if (byte == 0x15) { /* Ctrl-U */
                buf_reset(&question.text);
                question.empty_hint = 0;
            } else if (byte < 0x20) {
                ;
            } else {
                size_t sequence_len = utf8_sequence_length(byte);
                char bytes[4] = {(char)byte};
                size_t bytes_read = 1;
                for (size_t i = 1; i < sequence_len; i++) {
                    unsigned char continuation;
                    if (read_byte_timeout(&continuation, ESC_TIMEOUT_MS) <= 0)
                        break;
                    bytes[bytes_read++] = (char)continuation;
                }
                buf_append(&question.text, bytes, bytes_read);
                question.empty_hint = 0;
            }
        } else if (byte == 0x09) { /* Tab */
            question_move_tab(&question, questions, 1);
        } else if (byte == 0x0d || byte == 0x0a) { /* Enter */
            const struct ask_question *ask =
                question.tab < question.question_count ? &questions[question.tab] : NULL;
            if (ask) {
                size_t selectable = option_count(ask);
                if (selectable > 0) {
                    if (question.option_index >= selectable)
                        question.option_index = 0;
                    if (is_free_text_option(ask, question.option_index)) {
                        question.input_mode = 1;
                        question.empty_hint = 0;
                        buf_reset(&question.text);
                    } else {
                        question_select(&question, ask, question.option_index);
                        if (question_advance(&question, questions))
                            break;
                    }
                }
            } else {
                /* Submit tab: Enter completes only when every question is answered. */
                int all_answered = 1;
                for (size_t i = 0; i < question.question_count; i++)
                    if (!question.answers[i].has_answer)
                        all_answered = 0;
                if (all_answered)
                    break;
            }
        } else if (byte < 0x20) {
            ;
        }

        paint(&question, questions);
    }

    /* Erase the painted area; leave the cursor at column 0 of a clean line, and restore the
     * cursor. */
    if (question.painted && question.previous_row_count > 0) {
        int current_terminal_cols, current_terminal_rows;
        get_terminal_size(&current_terminal_cols, &current_terminal_rows);
        int climb = reflow_climb(&question, current_terminal_cols, current_terminal_rows);
        fputs(ANSI_SYNC_BEGIN, stdout);
        if (climb > 0)
            printf(ANSI_CSI "%dA", climb);
        fputs("\r" ANSI_ERASE_BELOW ANSI_SYNC_END, stdout);
    }
    fputs(ANSI_CURSOR_SHOW, stdout);
    fflush(stdout);
    disable_raw_mode(&question);

    if (cancelled) {
        for (size_t i = 0; i < question.question_count; i++) {
            free(question.answers[i].text);
            free(question.answers[i].value);
        }
        buf_free(&question.text);
        return 0;
    }

    size_t answer_count = 0;
    for (size_t i = 0; i < question.question_count; i++)
        if (question.answers[i].has_answer)
            answer_count++;

    result->answers = xmalloc((answer_count ? answer_count : 1) * sizeof(*result->answers));
    result->n_answers = 0;
    result->cancelled = 0;
    for (size_t i = 0; i < question.question_count; i++) {
        struct question_answer *answer = &question.answers[i];
        if (!answer->has_answer)
            continue;
        result->answers[result->n_answers].question_id = questions[i].id;
        result->answers[result->n_answers].index = answer->index;
        result->answers[result->n_answers].text = answer->text;
        result->answers[result->n_answers].value = answer->value;
        result->answers[result->n_answers].was_custom = answer->was_custom;
        result->n_answers++;
        answer->text = NULL;
        answer->value = NULL;
    }
    buf_free(&question.text);
    return 0;
}
