/* SPDX-License-Identifier: MIT */
#include "terminal/input.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
/* The wait macros are provided by <sys/wait.h> per POSIX; glibc also leaks
 * them through <stdlib.h>, so the include cleaner cannot attribute them. */
#include <sys/wait.h> // IWYU pragma: keep

#include "buf.h"
#include "xalloc.h"
#include "system/fd.h"
#include "system/fs.h"
#include "system/locale.h"
#include "system/path.h"
#include "system/spawn.h"
#include "terminal/ansi.h"
#include "terminal/input_core.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "terminal/width.h"
#include "text/utf8.h"
#include "text/utf8_sanitize.h"

#define ESC_TIMEOUT_MS 50

static void terminal_size(int *columns, int *rows)
{
    struct winsize size;

    *columns = 0;
    *rows = 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0)
        return;
    if (size.ws_col > 0)
        *columns = size.ws_col;
    if (size.ws_row > 0)
        *rows = size.ws_row;
}

static int editor_columns(int terminal_columns)
{
    int columns = display_width();

    if (terminal_columns > 0 && columns > terminal_columns)
        columns = terminal_columns;
    return columns > 0 ? columns : 1;
}

static void refresh_terminal_size(struct input *in)
{
    int terminal_columns;

    terminal_size(&terminal_columns, &in->terminal_rows);
    in->display_columns = editor_columns(terminal_columns);
}

int input_display_cols(void)
{
    int terminal_columns;
    int rows;

    terminal_size(&terminal_columns, &rows);
    return editor_columns(terminal_columns);
}

/* Raw mode */

static void enable_raw_mode(struct input *in)
{
    if (in->raw_active)
        return;
    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &in->saved_termios) < 0)
        return;

    struct termios raw = in->saved_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP | BRKINT);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
        return;

    fputs(ANSI_BRACKETED_PASTE_ENABLE, stdout);
    fflush(stdout);
    in->raw_active = 1;
}

static void disable_raw_mode(struct input *in)
{
    if (!in->raw_active)
        return;
    fputs(ANSI_BRACKETED_PASTE_DISABLE, stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSADRAIN, &in->saved_termios);
    in->raw_active = 0;
}

/* ---------------- input primitives ---------------- */

static int read_byte_blocking(unsigned char *out)
{
    for (;;) {
        ssize_t n = read(STDIN_FILENO, out, 1);
        if (n == 1)
            return 1;
        if (n == 0)
            return 0; /* EOF */
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static int read_byte_timeout(unsigned char *out, int timeout_ms)
{
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
    int result;

    do {
        result = poll(&input, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result <= 0)
        return result;
    return read_byte_blocking(out);
}

/* ---------------- bracketed paste ---------------- */

typedef void (*paste_sink_fn)(void *user, const char *bytes, size_t len);

/* Keep consuming after the size cap so paste bytes cannot leak into the keypress loop. */
static void read_bracketed_paste(paste_sink_fn sink, void *user)
{
    static const char end_marker[] = "\x1b[201~";
    const size_t end_marker_len = sizeof(end_marker) - 1;
    const size_t max_bytes = 1u << 20;
    const int idle_timeout_ms = 5000;
    char pending[sizeof(end_marker)];
    size_t pending_len = 0;
    size_t emitted_bytes = 0;
    int swallow_lf = 0;

    for (;;) {
        unsigned char byte;
        if (read_byte_timeout(&byte, idle_timeout_ms) <= 0)
            return;
        if (byte == '\n' && swallow_lf) {
            swallow_lf = 0;
            continue;
        }
        if (byte == '\r') {
            byte = '\n';
            swallow_lf = 1;
        } else {
            swallow_lf = 0;
        }
        if (byte == '\0')
            byte = ' ';
        pending[pending_len++] = (char)byte;

        for (;;) {
            size_t prefix_len = pending_len < end_marker_len ? pending_len : end_marker_len;
            if (memcmp(pending, end_marker, prefix_len) == 0) {
                if (pending_len == end_marker_len)
                    return;
                break;
            }
            if (emitted_bytes < max_bytes) {
                sink(user, pending, 1);
                emitted_bytes++;
            }
            memmove(pending, pending + 1, pending_len - 1);
            pending_len--;
            if (pending_len == 0)
                break;
        }
    }
}

static void append_paste_body(void *user, const char *bytes, size_t n)
{
    buf_append((struct buf *)user, bytes, n);
}

static void invoke_paste_hook(struct input *in)
{
    if (!in->paste_hook)
        return;
    char *ins = in->paste_hook(in->paste_hook_user);
    if (!ins)
        return;
    input_core_insert(in, ins, strlen(ins));
    free(ins);
}

/* Buffer the complete body for filtering. macOS uses an empty body when the clipboard
 * contains an image, so defer that case to the paste hook. */
static void handle_bracketed_paste(struct input *in)
{
    struct buf body;
    buf_init(&body);
    read_bracketed_paste(append_paste_body, &body);
    if (body.len == 0) {
        buf_free(&body);
        invoke_paste_hook(in);
        return;
    }
    input_core_commit_paste(in, body.data, body.len);
    buf_free(&body);
}

/* ---------------- escape sequence dispatch ---------------- */

static int read_escape_byte(void *user)
{
    (void)user;
    unsigned char b;
    int r = read_byte_timeout(&b, ESC_TIMEOUT_MS);
    if (r <= 0)
        return -1;
    return b;
}

static void apply_action(struct input *in, enum input_action a)
{
    switch (a) {
    case INPUT_ACTION_NONE:
        return;
    case INPUT_ACTION_MOVE_LEFT:
        input_core_move_left(in);
        return;
    case INPUT_ACTION_MOVE_RIGHT:
        input_core_move_right(in);
        return;
    case INPUT_ACTION_MOVE_WORD_LEFT:
        input_core_move_word_left(in);
        return;
    case INPUT_ACTION_MOVE_WORD_RIGHT:
        input_core_move_word_right(in);
        return;
    case INPUT_ACTION_LINE_START:
        in->cursor = input_core_line_start(in);
        return;
    case INPUT_ACTION_LINE_END:
        in->cursor = input_core_line_end(in);
        return;
    case INPUT_ACTION_DELETE_FWD:
        input_core_delete_fwd(in);
        return;
    case INPUT_ACTION_HISTORY_PREV:
        input_core_history_prev(in);
        return;
    case INPUT_ACTION_HISTORY_NEXT:
        input_core_history_next(in);
        return;
    case INPUT_ACTION_PAGE_UP:
    case INPUT_ACTION_PAGE_DOWN:
        /* No paged motion at the line prompt; consumed so the sequence
         * doesn't leak into the buffer as text. */
        return;
    case INPUT_ACTION_KILL_WORD_FWD:
        input_core_kill_word_fwd(in);
        return;
    case INPUT_ACTION_KILL_WORD_BACK_ALNUM:
        input_core_kill_word_back_alnum(in);
        return;
    case INPUT_ACTION_INSERT_NEWLINE:
        input_core_insert(in, "\n", 1);
        return;
    case INPUT_ACTION_PASTE_BEGIN:
        handle_bracketed_paste(in);
        return;
    }
}

static void handle_escape_sequence(struct input *in)
{
    apply_action(in, input_core_decode_escape(read_escape_byte, NULL));
}

/* ---------------- render / paint ---------------- */

static void buf_append_csi(struct buf *out, int value, char final)
{
    char sequence[24];
    int len = snprintf(sequence, sizeof(sequence), ANSI_CSI "%d%c", value, final);

    buf_append(out, sequence, (size_t)len);
}

/* Raw mode disables OPOST, so row breaks must include CR. A clipped window already
 * starts on its first visible row and needs only that row's indent. */
struct paint_context {
    struct buf *frame;
    int first_row;
};

static void paint_emit(const struct input_render_event *event, void *user)
{
    struct paint_context *context = user;

    if (event->kind == INPUT_RENDER_GLYPH) {
        buf_append(context->frame, event->bytes, event->n);
        return;
    }
    if (event->row > context->first_row)
        buf_append_str(context->frame, ANSI_ERASE_LINE "\r\n");
    for (int column = 0; column < event->col; column++)
        buf_append(context->frame, " ", 1);
}

/* Omit an indicator that would wrap because paint accounts for it as one row. Return
 * its width so resize recovery can account for later terminal reflow. */
static int append_clip_indicator(struct buf *frame, int hidden_rows, int columns)
{
    int painted_width = 0;

    if (hidden_rows > 0) {
        int have_utf8 = locale_have_utf8();
        char *indicator = xasprintf("%s +%d line%s", have_utf8 ? "\xe2\x80\xa6" : "...",
                                    hidden_rows, hidden_rows == 1 ? "" : "s");
        int width = (int)strlen(indicator) - (have_utf8 ? 2 : 0);
        if (width < columns) {
            buf_append_str(frame, ANSI_DIM);
            buf_append_str(frame, indicator);
            buf_append_str(frame, ANSI_BOLD_OFF);
            painted_width = width;
        }
        free(indicator);
    }
    buf_append_str(frame, ANSI_ERASE_LINE);
    return painted_width;
}

/* Keep the prompt compact while reserving two rows for clipping indicators. */
static int edit_area_rows(int terminal_rows)
{
    int cap = terminal_rows * 2 / 5;
    if (cap < 3)
        cap = 3;
    if (cap > terminal_rows)
        cap = terminal_rows;
    return cap;
}

/* Batch each repaint under DEC synchronized output and draw before clearing stale rows,
 * avoiding partial or blank frames on terminals that ignore synchronization. Clip tall buffers
 * because relative cursor motion cannot reach rows pushed into scrollback; clipping requires two
 * indicator rows and at least one content row. */
static void paint(struct input *in)
{
    int prompt_width = input_core_prompt_width(in->prompt);
    int continuation_column = in->continuation_at_column_zero ? 0 : prompt_width;
    struct input_layout layout;

    input_core_render(in->buf, in->len, in->cursor, prompt_width, continuation_column,
                      in->display_columns, NULL, NULL, &layout);

    int row_limit = edit_area_rows(in->terminal_rows);
    int clipped = in->terminal_rows >= 3 && layout.total_rows > row_limit;
    int first_row = 0;
    int visible_rows = layout.total_rows;
    if (clipped) {
        visible_rows = row_limit - 2;
        first_row = input_core_window_top(in->window_top, layout.cursor_row, layout.total_rows,
                                          visible_rows);
    }

    struct buf frame;
    buf_init(&frame);
    buf_append_str(&frame, ANSI_SYNC_BEGIN);

    if (in->painted_cursor_row > 0)
        buf_append_csi(&frame, in->painted_cursor_row, 'A');
    buf_append(&frame, "\r", 1);

    int top_indicator_width = 0;
    if (clipped) {
        top_indicator_width = append_clip_indicator(&frame, first_row, in->display_columns);
        buf_append_str(&frame, "\r\n");
    }
    if (first_row == 0)
        buf_append_str(&frame, in->prompt);

    struct paint_context context = {.frame = &frame, .first_row = first_row};
    if (clipped) {
        input_core_render_window(in->buf, in->len, in->cursor, prompt_width, continuation_column,
                                 in->display_columns, first_row, first_row + visible_rows - 1,
                                 paint_emit, &context, NULL);
    } else {
        input_core_render(in->buf, in->len, in->cursor, prompt_width, continuation_column,
                          in->display_columns, paint_emit, &context, NULL);
    }

    /* Ghost text after the empty prompt; the cursor repositioning below lands on top of it. */
    in->hint_painted = 0;
    if (in->exit_armed && in->len == 0) {
        static const char exit_hint[] = "ctrl+c again to exit";
        if (prompt_width + (int)sizeof(exit_hint) - 1 <= in->display_columns) {
            buf_append_str(&frame, ANSI_DIM);
            buf_append_str(&frame, exit_hint);
            buf_append_str(&frame, ANSI_BOLD_OFF);
            in->hint_painted = 1;
        }
    }

    if (clipped) {
        buf_append_str(&frame, ANSI_ERASE_LINE "\r\n");
        append_clip_indicator(&frame, layout.total_rows - first_row - visible_rows,
                              in->display_columns);
    }
    buf_append_str(&frame, ANSI_ERASE_BELOW);

    int cursor_screen_row = clipped ? layout.cursor_row - first_row + 1 : layout.cursor_row;
    int end_screen_row = clipped ? visible_rows + 1 : layout.end_row;
    int rows_up = end_screen_row - cursor_screen_row;
    if (rows_up > 0)
        buf_append_csi(&frame, rows_up, 'A');
    buf_append(&frame, "\r", 1);
    if (layout.cursor_col > 0)
        buf_append_csi(&frame, layout.cursor_col, 'C');

    buf_append_str(&frame, ANSI_SYNC_END);
    fwrite(frame.data, 1, frame.len, stdout);
    fflush(stdout);
    buf_free(&frame);

    in->painted_cursor_row = cursor_screen_row;
    in->painted_rows = clipped ? visible_rows + 2 : layout.total_rows;
    in->previous_paint_clipped = clipped;
    in->window_top = first_row;
    in->top_indicator_width = top_indicator_width;
}

/* Record row widths under the old geometry for resize reflow calculations. */
struct row_widths {
    int *values;
    int capacity;
    int count;
    int current;
};

static void collect_row_width(const struct input_render_event *event, void *user)
{
    struct row_widths *widths = user;

    if (event->kind == INPUT_RENDER_GLYPH) {
        int end_column = event->col + event->width;
        if (end_column > widths->current)
            widths->current = end_column;
        return;
    }
    if (widths->count < widths->capacity)
        widths->values[widths->count] = widths->current;
    widths->count++;
    widths->current = event->col;
}

/* On width changes, rewalk the old layout to account for terminals that reflow each
 * painted row into multiple physical rows. This must run before applying the next key so the
 * saved paint state still describes the screen. Terminals that truncate instead of reflowing
 * may briefly over-clear or leave one stale row. */
static int handle_resize(struct input *in)
{
    int terminal_columns;
    int new_rows;

    terminal_size(&terminal_columns, &new_rows);
    int new_columns = editor_columns(terminal_columns);
    if (new_columns == in->display_columns && new_rows == in->terminal_rows)
        return 0;

    int old_columns = in->display_columns;
    in->display_columns = new_columns;
    in->terminal_rows = new_rows;
    if (new_columns == old_columns) {
        if (new_rows > 0 && in->painted_cursor_row >= new_rows)
            in->painted_cursor_row = new_rows - 1;
        return 1;
    }

    int cursor_climb = 0;
    if (in->painted_rows > 0 && new_columns > 0) {
        int prompt_width = input_core_prompt_width(in->prompt);
        int capacity = in->window_top + in->painted_rows + 1;
        struct row_widths widths = {
            .values = xcalloc((size_t)capacity, sizeof(int)),
            .capacity = capacity,
            .current = prompt_width,
        };
        struct input_layout layout;
        int continuation_column = in->continuation_at_column_zero ? 0 : prompt_width;

        input_core_render(in->buf, in->len, in->cursor, prompt_width, continuation_column,
                          old_columns, collect_row_width, &widths, &layout);
        if (widths.count < widths.capacity)
            widths.values[widths.count] = widths.current;
        widths.count++;

        int first_row = in->previous_paint_clipped ? in->window_top : 0;
        if (in->previous_paint_clipped) {
            cursor_climb = in->top_indicator_width > 0
                               ? (in->top_indicator_width + new_columns - 1) / new_columns
                               : 1;
        }
        int last_row = layout.cursor_row < widths.count ? layout.cursor_row : widths.count;
        if (last_row > first_row)
            cursor_climb +=
                reflow_physical_rows(widths.values + first_row, last_row - first_row, new_columns);
        cursor_climb += layout.cursor_col / new_columns;
        free(widths.values);

        /* A terminal cannot retain more rows above the cursor than its viewport. */
        if (new_rows > 0 && cursor_climb >= new_rows)
            cursor_climb = new_rows - 1;
    }

    in->painted_cursor_row = cursor_climb;
    in->painted_rows = 0;
    return 1;
}

static void leave_edit_area(struct input *in)
{
    int down = in->painted_rows - 1 - in->painted_cursor_row;
    if (down > 0)
        printf(ANSI_CSI "%dB", down);
    fputs("\r\n", stdout);
    fflush(stdout);
    in->painted_cursor_row = 0;
    in->painted_rows = 0;
}

/* Reset the accent around each continuation stripe so attributes do not nest. */
static void submitted_emit(const struct input_render_event *event, void *user)
{
    struct buf *frame = user;

    if (event->kind == INPUT_RENDER_GLYPH) {
        buf_append(frame, event->bytes, event->n);
    } else {
        buf_append_str(frame, theme_close(THEME_ACCENT));
        buf_append_str(frame, ANSI_ERASE_LINE "\r\n");
        buf_append_str(frame, theme_open(THEME_ACCENT));
        buf_append_str(frame, "▌ ");
    }
}

static void append_user_message(struct buf *frame, const char *text, size_t len,
                                int display_columns)
{
    const int body_column = 2;

    buf_append_str(frame, theme_open(THEME_ACCENT));
    buf_append_str(frame, "▌ ");
    input_core_render(text, len, 0, body_column, body_column, display_columns, submitted_emit,
                      frame, NULL);
    buf_append_str(frame, theme_close(THEME_ACCENT));
    buf_append_str(frame, ANSI_ERASE_LINE "\r\n");
}

void input_render_user_message_to(FILE *out, const char *text, size_t len, int display_columns)
{
    struct buf frame;

    buf_init(&frame);
    append_user_message(&frame, text, len, display_columns);
    fwrite(frame.data, 1, frame.len, out);
    fflush(out);
    buf_free(&frame);
}

/* Draw before erasing stale rows so terminals without synchronization do not flash. */
static void render_submitted(struct input *in)
{
    struct buf frame;

    buf_init(&frame);
    buf_append_str(&frame, ANSI_SYNC_BEGIN);
    if (in->painted_cursor_row > 0)
        buf_append_csi(&frame, in->painted_cursor_row, 'A');
    buf_append(&frame, "\r", 1);
    append_user_message(&frame, in->buf, in->len, in->display_columns);
    buf_append_str(&frame, ANSI_ERASE_BELOW);
    buf_append_str(&frame, ANSI_SYNC_END);
    fwrite(frame.data, 1, frame.len, stdout);
    fflush(stdout);
    buf_free(&frame);

    in->painted_cursor_row = 0;
    in->painted_rows = 0;
}

/* Erase line-by-line because erase-below from the top of the screen can push a stale
 * screen into scrollback in tmux and some terminals. Cursor-down clamps instead of scrolling. */
static void erase_edit_area(struct input *in)
{
    struct buf frame;
    int rows = in->painted_rows > 0 ? in->painted_rows : 1;

    buf_init(&frame);
    if (in->painted_cursor_row > 0)
        buf_append_csi(&frame, in->painted_cursor_row, 'A');
    buf_append(&frame, "\r", 1);
    for (int row = 0; row < rows; row++) {
        buf_append_str(&frame, ANSI_ERASE_LINE);
        if (row + 1 < rows)
            buf_append_csi(&frame, 1, 'B');
    }
    if (rows > 1)
        buf_append_csi(&frame, rows - 1, 'A');
    fwrite(frame.data, 1, frame.len, stdout);
    fflush(stdout);
    buf_free(&frame);
    in->painted_cursor_row = 0;
    in->painted_rows = 0;
    in->hint_painted = 0;
}

/* ---------------- $EDITOR escape ---------------- */

/* Probe candidates because minimal installs ship few or no editors; `editor` is Debian's
 * alternatives name. A broken $VISUAL/$EDITOR is an error, not a fallback trigger — silently
 * substituting would hide it. Returns NULL after reporting why. */
static const char *resolve_editor(void)
{
    static const char *const FALLBACKS[] = {"editor", "nano", "vim", "vi"};

    const char *source = "$VISUAL";
    const char *configured = getenv("VISUAL");
    if (!configured || !*configured) {
        source = "$EDITOR";
        configured = getenv("EDITOR");
    }
    if (configured && *configured) {
        if (fs_shell_head_resolves(configured))
            return configured;
        ui_error("%s (%s) not found — fix it, or unset it to use a fallback", source, configured);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(FALLBACKS) / sizeof(FALLBACKS[0]); i++) {
        char *found = fs_which(FALLBACKS[i]);
        if (found) {
            free(found);
            return FALLBACKS[i];
        }
    }
    ui_error("no editor found — install one or set $EDITOR");
    return NULL;
}

static void open_editor(struct input *in)
{
    /* Alternate-screen editors restore the cursor to this cleared position. */
    erase_edit_area(in);
    disable_raw_mode(in);

    const char *editor = resolve_editor();
    if (!editor) {
        /* No disp in modal context; restore the block gap before the repainted prompt. */
        putchar('\n');
        goto reenter;
    }

    char path[] = "/tmp/hax-edit-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        goto reenter;
    if (in->len > 0 && fd_write_all(fd, in->buf, in->len) < 0) {
        close(fd);
        unlink(path);
        goto reenter;
    }
    close(fd);

    /* The mkstemp path is shell-safe; VISUAL/EDITOR is trusted user configuration and the
     * probed fallbacks are bare command names. */
    /* The buffer handed over is whatever the user typed or pasted, and comes back the same way, so
     * the editor has to agree with hax about the encoding or it will rewrite the text. */
    char *cmd = spawn_shell_cmd_force_utf8(xasprintf("%s '%s'", editor, path));
    int status = spawn_shell_wait(cmd);
    free(cmd);

    int aborted = status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;

    size_t n = 0;
    char *content = aborted ? NULL : fs_read_file(path, &n);
    unlink(path);
    if (content) {
        /* The edit buffer is NUL-terminated; preserve embedded NUL positions as spaces. */
        for (size_t k = 0; k < n; k++) {
            if (content[k] == '\0')
                content[k] = ' ';
        }
        /* Drop one trailing newline — most editors append one automatically. */
        if (n > 0 && content[n - 1] == '\n')
            content[--n] = '\0';
        input_core_set_buffer(in, content);
        free(content);
    }

reenter:
    enable_raw_mode(in);
    refresh_terminal_size(in);
}

/* ---------------- Modal control keys ---------------- */

static int run_modal_key(struct input *in, unsigned char key)
{
    void (*fn)(void *user) = NULL;
    void *user = NULL;

    for (size_t i = 0; i < INPUT_MODAL_KEYS_MAX; i++) {
        if (in->modal_keys[i].fn && in->modal_keys[i].key == key) {
            fn = in->modal_keys[i].fn;
            user = in->modal_keys[i].user;
            break;
        }
    }
    if (!fn)
        return 0;

    erase_edit_area(in);
    disable_raw_mode(in);

    fn(user);

    enable_raw_mode(in);
    refresh_terminal_size(in);
    return 1;
}

/* ---------------- Tab modal completion ---------------- */

static void complete_modal(struct input *in, size_t start, size_t end)
{
    size_t token_len = end - start;
    char *token = xmalloc(token_len + 1);

    if (token_len > 0)
        memcpy(token, in->buf + start, token_len);
    token[token_len] = '\0';

    erase_edit_area(in);
    disable_raw_mode(in);

    char *replacement = in->completer->pick(token, in->completer->user);
    free(token);

    enable_raw_mode(in);
    refresh_terminal_size(in);
    if (replacement && *replacement)
        input_core_replace_span(in, start, end, replacement);
    free(replacement);
}

/* ---------------- reverse / forward incremental search ---------------- */

enum history_search_outcome {
    HISTORY_SEARCH_ACCEPT,
    HISTORY_SEARCH_SUBMIT,
};

enum history_search_direction {
    HISTORY_SEARCH_OLDER = -1,
    HISTORY_SEARCH_NEWER = 1,
};

/* A history-search query is single-line, so pasted control bytes are discarded. */
static void paste_into_query(void *user, const char *bytes, size_t len)
{
    struct buf *query = user;

    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if (byte >= 0x20 && byte != 0x7f)
            buf_append(query, &bytes[i], 1);
    }
}

static void recompute_history_match(struct input *in, const struct buf *query,
                                    enum history_search_direction direction, long *match,
                                    int *no_match)
{
    if (query->len == 0) {
        *match = -1;
        *no_match = 0;
        return;
    }

    long start = *match >= 0 ? *match : direction < 0 ? (long)in->hist_n - 1 : 0;
    long found = input_core_history_search(in, query->data, start, direction);
    if (found >= 0) {
        *match = found;
        *no_match = 0;
    } else {
        *no_match = 1;
    }
}

/* Search prompts bypass the render walker, so sanitize untrusted query bytes here. */
static char *sanitize_query_for_display(const char *query)
{
    size_t len = strlen(query);
    struct buf sanitized;

    buf_init(&sanitized);
    for (size_t offset = 0; offset < len;) {
        unsigned char byte = (unsigned char)query[offset];
        if (byte < 0x20 || byte == 0x7f) {
            buf_append(&sanitized, "?", 1);
            offset++;
            continue;
        }

        size_t consumed;
        int width = utf8_codepoint_cells(query, len, offset, &consumed);
        if (width < 0) {
            buf_append(&sanitized, "?", 1);
            offset += consumed ? consumed : 1;
            continue;
        }
        consumed = consumed ? consumed : 1;
        buf_append(&sanitized, query + offset, consumed);
        offset += consumed;
    }
    return buf_steal(&sanitized);
}

/* Keep the tail because it contains the portion of the query being edited. */
static char *clip_query_left(const char *query, int available_columns, int have_utf8)
{
    size_t len = strlen(query);
    int total_width = 0;

    for (size_t offset = 0; offset < len;) {
        size_t consumed;
        int width = utf8_codepoint_cells(query, len, offset, &consumed);
        total_width += width < 0 ? 1 : width;
        offset += consumed ? consumed : 1;
    }
    if (available_columns < 1)
        return xstrdup("");
    if (total_width <= available_columns)
        return xstrdup(query);

    const char *marker = have_utf8 ? "\xe2\x80\xa6" : "<";
    int budget = available_columns - 1;
    size_t keep_from = len;
    int kept_width = 0;
    for (size_t offset = len; offset > 0;) {
        size_t previous = utf8_prev(query, offset);
        size_t consumed;
        int width = utf8_codepoint_cells(query, len, previous, &consumed);
        if (width < 0)
            width = 1;
        if (kept_width + width > budget)
            break;
        kept_width += width;
        keep_from = previous;
        offset = previous;
    }
    return xasprintf("%s%s", marker, query + keep_from);
}

static void show_history_search_match(struct input *in, const char *original,
                                      size_t original_cursor, const struct buf *query, long match,
                                      int no_match)
{
    if (no_match) {
        input_core_set_buffer(in, "");
        return;
    }
    if (match < 0) {
        input_core_set_buffer(in, original);
        in->cursor = original_cursor <= in->len ? original_cursor : in->len;
        return;
    }

    input_core_set_buffer(in, in->hist[match]);
    char *match_start = query->len > 0 ? strstr(in->buf, query->data) : NULL;
    in->cursor = match_start ? (size_t)(match_start - in->buf) : in->len;
}

/* The search prompt must remain one row because paint tracks only rendered buffer rows. */
static char *build_history_search_prompt(const struct buf *query,
                                         enum history_search_direction direction, int no_match,
                                         int display_columns)
{
    int have_utf8 = locale_have_utf8();
    const char *label = direction == HISTORY_SEARCH_OLDER ? "reverse-search" : "forward-search";
    const char *separator = query->len > 0 ? (have_utf8 ? " \xc2\xb7 " : " : ") : "";
    const char *arrow = have_utf8 ? " \xe2\x86\x92 " : " > ";
    char *safe_query = query->len > 0 ? sanitize_query_for_display(query->data) : NULL;
    int budget = display_columns > 1 ? display_columns - 1 : 1;
    int fixed_width = (int)strlen(label) + (query->len > 0 ? 3 : 0) + 3 + (no_match ? 10 : 0);
    char *prompt;

    if (fixed_width <= budget) {
        char *visible_query =
            safe_query ? clip_query_left(safe_query, budget - fixed_width, have_utf8) : NULL;
        const char *suffix = no_match ? ANSI_DIM "(no match)" ANSI_BOLD_OFF : "";
        prompt =
            xasprintf("%s%s%s%s%s%s%s", theme_open(THEME_ACCENT), label, separator,
                      visible_query ? visible_query : "", arrow, theme_close(THEME_ACCENT), suffix);
        free(visible_query);
    } else {
        char *plain = xasprintf("%s%s%s%s%s", label, separator, safe_query ? safe_query : "", arrow,
                                no_match ? "(no match)" : "");
        char *visible = clip_query_left(plain, budget, have_utf8);
        prompt = xasprintf("%s%s%s", theme_open(THEME_ACCENT), visible, theme_close(THEME_ACCENT));
        free(visible);
        free(plain);
    }
    free(safe_query);
    return prompt;
}

static enum history_search_outcome search_history(struct input *in)
{
    char *original = xstrdup(in->buf);
    size_t original_cursor = in->cursor;
    size_t original_history_position = in->hist_pos;
    const char *original_prompt = in->prompt;
    struct buf query;
    long match = -1;
    int no_match = 0;
    enum history_search_direction direction = HISTORY_SEARCH_OLDER;
    int accepted = 0;
    char *search_prompt = NULL;
    enum history_search_outcome outcome = HISTORY_SEARCH_ACCEPT;

    buf_init(&query);
    in->continuation_at_column_zero = 1;

    for (;;) {
        show_history_search_match(in, original, original_cursor, &query, match, no_match);
        free(search_prompt);
        search_prompt =
            build_history_search_prompt(&query, direction, no_match, in->display_columns);
        in->prompt = search_prompt;
        paint(in);

        unsigned char key;
        if (read_byte_blocking(&key) <= 0)
            break;

        handle_resize(in);

        if (key == 0x12 || key == 0x13) {
            direction = key == 0x12 ? HISTORY_SEARCH_OLDER : HISTORY_SEARCH_NEWER;
            if (query.len == 0 || no_match)
                continue;
            long found = input_core_history_search(in, query.data, match + direction, direction);
            if (found >= 0)
                match = found;
            continue;
        }
        if (key == 0x7f || key == 0x08) {
            if (query.len > 0) {
                query.len = utf8_prev(query.data, query.len);
                query.data[query.len] = '\0';
            }
            recompute_history_match(in, &query, direction, &match, &no_match);
            continue;
        }
        if (key == 0x07 || key == 0x03)
            break;
        if (key == 0x0d) {
            accepted = 1;
            outcome = HISTORY_SEARCH_SUBMIT;
            goto done;
        }
        if (key == 0x0a) {
            accepted = 1;
            goto done;
        }
        if (key == 0x1b) {
            if (input_core_decode_escape(read_escape_byte, NULL) == INPUT_ACTION_PASTE_BEGIN) {
                read_bracketed_paste(paste_into_query, &query);
                recompute_history_match(in, &query, direction, &match, &no_match);
                continue;
            }
            accepted = 1;
            goto done;
        }
        if (key >= 0x20) {
            size_t sequence_len = utf8_sequence_length(key);
            char bytes[4] = {(char)key};
            size_t bytes_read = 1;
            for (size_t i = 1; i < sequence_len; i++) {
                unsigned char byte;
                if (read_byte_timeout(&byte, ESC_TIMEOUT_MS) <= 0)
                    break;
                bytes[bytes_read++] = (char)byte;
            }
            buf_append(&query, bytes, bytes_read);
            recompute_history_match(in, &query, direction, &match, &no_match);
            continue;
        }
    }

    show_history_search_match(in, original, original_cursor, &query, -1, 0);

done:
    if (accepted && !no_match && match >= 0) {
        show_history_search_match(in, original, original_cursor, &query, match, 0);
        in->hist_pos = (size_t)match;
        /* Preserve a live draft for Down past the newest search result. */
        if (original_history_position == in->hist_n) {
            free(in->draft);
            in->draft = xstrdup(original);
        }
    } else if (accepted) {
        show_history_search_match(in, original, original_cursor, &query, -1, 0);
        outcome = HISTORY_SEARCH_ACCEPT;
    }
    in->continuation_at_column_zero = 0;
    in->prompt = original_prompt;
    free(search_prompt);
    buf_free(&query);
    free(original);
    return outcome;
}

/* ---------------- non-tty fallback ---------------- */

static char *read_line_canonical(size_t *out_len)
{
    /* Byte-wise reads preserve embedded NULs until utf8_sanitize can replace them. */
    struct buf b;
    buf_init(&b);
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF)
            break;
        if (ch == '\n') {
            *out_len = b.len;
            return buf_steal(&b);
        }
        char c = (char)ch;
        buf_append(&b, &c, 1);
    }
    if (b.len > 0) {
        *out_len = b.len;
        return buf_steal(&b);
    }
    buf_free(&b);
    *out_len = 0;
    return NULL;
}

/* ---------------- history persistence ---------------- */

/* Compact append-only history only after it substantially exceeds the in-memory cap. */
#define HISTORY_FILE_BLOAT_FACTOR 3

/* Bound both writes and startup allocation when history is corrupt or hand-edited. */
#define HISTORY_RECORD_MAX 65536

/* Use one O_APPEND write so concurrent processes cannot interleave a record. Retrying a
 * short write would lose that property, so a rare short write is allowed to truncate history. */
static void history_file_append(const char *path, const char *line)
{
    char *enc = input_core_history_encode(line);
    size_t n = strlen(enc);
    if (n + 1 > HISTORY_RECORD_MAX) {
        free(enc);
        return;
    }

    /* O_NONBLOCK makes opening a planted FIFO safe; fstat closes the replacement race. */
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0600);
    struct stat status;
    if (fd < 0 || fstat(fd, &status) < 0 || !S_ISREG(status.st_mode)) {
        if (fd >= 0)
            close(fd);
        free(enc);
        return;
    }

    char *rec = xmalloc(n + 1);
    memcpy(rec, enc, n);
    rec[n] = '\n';
    ssize_t w;
    do {
        w = write(fd, rec, n + 1);
    } while (w < 0 && errno == EINTR);
    free(rec);
    free(enc);
    close(fd);
}

/* A sibling temporary file prevents partial reads. Concurrent appends during rename may be
 * lost; compaction runs only at startup after the bloat threshold. */
static void history_file_rewrite(struct input *in, const char *path)
{
    char *dup = xstrdup(path);
    char *tmp = xasprintf("%s/.hax-hist.XXXXXX", dirname(dup));
    int fd = mkstemp(tmp);
    if (fd < 0) {
        free(tmp);
        free(dup);
        return;
    }
    (void)fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        free(tmp);
        free(dup);
        return;
    }
    for (size_t i = 0; i < in->hist_n; i++) {
        char *enc = input_core_history_encode(in->hist[i]);
        fputs(enc, f);
        fputc('\n', f);
        free(enc);
    }
    int ok = (fflush(f) == 0);
    if (fclose(f) != 0)
        ok = 0;
    if (ok && rename(tmp, path) != 0)
        ok = 0;
    if (!ok)
        unlink(tmp);
    free(tmp);
    free(dup);
}

/* Return records seen, including entries later evicted from the in-memory cap. */
static size_t history_file_load(struct input *in, const char *path)
{
    int fd = fs_open_regular(path);
    FILE *f = fd >= 0 ? fdopen(fd, "r") : NULL;
    if (fd >= 0 && !f)
        close(fd);

    size_t loaded = 0;
    if (f) {
        /* Drop oversized records but consume through newline to resynchronize the file. */
        char *line = xmalloc(HISTORY_RECORD_MAX);
        for (;;) {
            size_t n = 0;
            int c, overflow = 0;
            while ((c = fgetc(f)) != EOF && c != '\n') {
                if (n + 1 < HISTORY_RECORD_MAX)
                    line[n++] = (char)c;
                else
                    overflow = 1;
            }
            if (c == EOF && n == 0 && !overflow)
                break;
            loaded++;
            if (overflow)
                continue;
            while (n > 0 && line[n - 1] == '\r')
                n--;
            if (n == 0)
                continue;
            char *decoded = input_core_history_decode(line, n);
            input_core_history_add(in, decoded);
            free(decoded);
            if (c == EOF)
                break;
        }
        free(line);
        fclose(f);
    }
    return loaded;
}

void input_history_load(struct input *in, const char *path)
{
    if (!path || !*path)
        return;
    history_file_load(in, path);
}

void input_history_open(struct input *in, const char *path)
{
    if (!path || !*path)
        return;
    char *dup = xstrdup(path);
    fs_mkdir_p(dirname(dup));
    free(dup);

    size_t loaded = history_file_load(in, path);

    /* A failed compaction must not disable later appends. */
    free(in->persist_path);
    in->persist_path = xstrdup(path);

    if (loaded > (size_t)INPUT_CORE_HISTORY_MAX * HISTORY_FILE_BLOAT_FACTOR)
        history_file_rewrite(in, path);
}

void input_history_add(struct input *in, const char *line)
{
    if (!line || !*line)
        return;
    /* A non-empty line that leaves history unchanged is a repeat of the newest
     * entry; it still needs the append when that entry was session-only. */
    if (!input_core_history_add(in, line) && !in->hist_newest_unpersisted)
        return;
    in->hist_newest_unpersisted = 0;
    if (in->persist_path)
        history_file_append(in->persist_path, line);
}

void input_history_add_session(struct input *in, const char *line)
{
    if (!line || !*line)
        return;
    if (input_core_history_add(in, line))
        in->hist_newest_unpersisted = 1;
}

int input_bind_modal_key(struct input *in, unsigned char key, void (*fn)(void *user), void *user)
{
    if (key >= 0x20)
        return -1;
    struct input_modal_key *slot = NULL;
    for (size_t i = 0; i < INPUT_MODAL_KEYS_MAX; i++) {
        if (in->modal_keys[i].fn && in->modal_keys[i].key == key) {
            slot = &in->modal_keys[i];
            break;
        }
    }
    if (!slot && fn) {
        for (size_t i = 0; i < INPUT_MODAL_KEYS_MAX; i++) {
            if (!in->modal_keys[i].fn) {
                slot = &in->modal_keys[i];
                break;
            }
        }
        if (!slot)
            return -1;
    }
    if (!slot)
        return 0;
    slot->key = key;
    slot->fn = fn;
    slot->user = user;
    return 0;
}

void input_set_modal_completer(struct input *in, const struct input_modal_completer *completer)
{
    in->completer = completer;
}

void input_set_paste_hook(struct input *in, char *(*fn)(void *user), void *user)
{
    in->paste_hook = fn;
    in->paste_hook_user = user;
}

void input_set_paste_filter(struct input *in, char *(*fn)(const char *text, void *user), void *user)
{
    in->paste_filter = fn;
    in->paste_filter_user = user;
}

void input_set_empty_submit(struct input *in, int enabled)
{
    in->empty_submit = enabled;
}

void input_set_preseed(struct input *in, const char *text)
{
    free(in->preseed);
    in->preseed = (text && *text) ? xstrdup(text) : NULL;
}

void input_history_open_default(struct input *in, int persist)
{
    /* Never retain piped input; it may contain secrets from unattended scripts. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;
    char *path = xdg_hax_state_path("history");
    if (!path)
        return;
    if (persist)
        input_history_open(in, path);
    else
        input_history_load(in, path);
    free(path);
}

/* ---------------- public API ---------------- */

char *input_readline(struct input *in, const char *prompt)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        /* Prevent a stale seed from reaching a later interactive read. */
        free(in->preseed);
        in->preseed = NULL;
        size_t n;
        char *raw = read_line_canonical(&n);
        if (!raw)
            return NULL;
        char *clean = utf8_sanitize(raw, n);
        free(raw);
        return clean;
    }

    in->prompt = prompt;
    refresh_terminal_size(in);
    in->len = 0;
    in->cursor = 0;
    in->buf[0] = '\0';
    if (in->preseed) {
        input_core_set_buffer(in, in->preseed);
        free(in->preseed);
        in->preseed = NULL;
    }
    in->hist_pos = in->hist_n;
    free(in->draft);
    in->draft = NULL;
    in->exit_armed = 0;
    in->painted_cursor_row = 0;
    in->painted_rows = 0;
    in->previous_paint_clipped = 0;
    in->window_top = 0;
    in->top_indicator_width = 0;

    fflush(stdout);
    enable_raw_mode(in);
    paint(in);

    int submit = 0;
    int eof = 0;

    while (!submit && !eof) {
        unsigned char c;
        int r = read_byte_blocking(&c);
        if (r <= 0) {
            eof = 1;
            break;
        }

        /* Resize recovery needs the buffer state that produced the current screen. */
        if (handle_resize(in))
            paint(in);

        if (c != 0x03)
            in->exit_armed = 0;

        switch (c) {
        case 0x01: /* Ctrl-A */
            in->cursor = input_core_line_start(in);
            break;
        case 0x02: /* Ctrl-B */
            input_core_move_left(in);
            break;
        case 0x03: /* Ctrl-C — clear the buffer; twice on empty quits */
            if (in->len > 0) {
                /* Keep the discarded draft recallable via Up, in memory only. */
                input_history_add_session(in, in->buf);
                in->hist_pos = in->hist_n;
                free(in->draft);
                in->draft = NULL;
                input_core_set_buffer(in, "");
            } else if (in->exit_armed) {
                eof = 1;
            } else {
                in->exit_armed = 1;
            }
            break;
        case 0x04: /* Ctrl-D — EOF on empty, else delete-fwd */
            if (in->len == 0) {
                eof = 1;
            } else {
                input_core_delete_fwd(in);
            }
            break;
        case 0x05: /* Ctrl-E */
            in->cursor = input_core_line_end(in);
            break;
        case 0x06: /* Ctrl-F */
            input_core_move_right(in);
            break;
        case 0x07: /* Ctrl-G — open $EDITOR, replace buffer, keep editing */
            open_editor(in);
            break;
        case 0x08: /* Ctrl-H */
        case 0x7f: /* DEL / backspace */
            input_core_delete_back(in);
            break;
        case 0x09: { /* Tab */
            size_t cs, ce;
            if (in->completer &&
                in->completer->match(in->buf, in->len, in->cursor, &cs, &ce, in->completer->user) &&
                cs <= ce && ce <= in->len)
                complete_modal(in, cs, ce);
            else
                input_core_insert(in, "\t", 1);
            break;
        }
        case 0x0a: /* LF — Shift+Enter inserts a newline */
            input_core_insert(in, "\n", 1);
            break;
        case 0x0b: /* Ctrl-K */
            input_core_kill_to_eol(in);
            break;
        case 0x0c: /* Ctrl-L — clear screen + repaint */
            fputs(ANSI_ERASE_SCREEN ANSI_CURSOR_HOME, stdout);
            in->painted_cursor_row = 0;
            in->painted_rows = 0;
            break;
        case 0x0d: /* CR — Enter; empty requires empty_submit */
            if (in->len > 0 || in->empty_submit)
                submit = 1;
            break;
        case 0x0e: /* Ctrl-N */
            input_core_history_next(in);
            break;
        case 0x10: /* Ctrl-P */
            input_core_history_prev(in);
            break;
        case 0x12: /* Ctrl-R — incremental reverse history search */
            if (search_history(in) == HISTORY_SEARCH_SUBMIT && in->len > 0)
                submit = 1;
            break;
        case 0x15: /* Ctrl-U */
            input_core_kill_to_bol(in);
            break;
        case 0x16: /* Ctrl-V */
            invoke_paste_hook(in);
            break;
        case 0x17: /* Ctrl-W */
            input_core_kill_word_back(in);
            break;
        case 0x1a: /* Ctrl-Z; raw mode disables the tty's ISIG handling. */
            leave_edit_area(in);
            disable_raw_mode(in);
            raise(SIGTSTP);
            enable_raw_mode(in);
            refresh_terminal_size(in);
            break;
        case 0x1b: /* ESC — start of escape sequence */
            handle_escape_sequence(in);
            break;
        default:
            if (c >= 0x20) {
                /* Time out malformed leaders because raw mode would consume Ctrl-C/D as
                 * continuation bytes. Partial sequences render as a substitute glyph. */
                size_t sequence_len = utf8_sequence_length(c);
                char bytes[4] = {(char)c};
                size_t bytes_read = 1;
                for (size_t i = 1; i < sequence_len; i++) {
                    unsigned char byte;
                    if (read_byte_timeout(&byte, ESC_TIMEOUT_MS) <= 0)
                        break;
                    bytes[bytes_read++] = (char)byte;
                }
                input_core_insert(in, bytes, bytes_read);
            } else {
                /* Built-in editing keys take precedence over application bindings. */
                run_modal_key(in, c);
            }
            break;
        }

        if (!eof && !submit)
            paint(in);
    }

    /* A key that ends the loop skips the disarm repaint, so a painted hint
     * would outlive the editor; erase it before leaving. */
    if (in->hint_painted) {
        in->exit_armed = 0;
        paint(in);
    }

    if (submit && in->len > 0)
        render_submitted(in);
    else
        leave_edit_area(in);
    disable_raw_mode(in);

    if (eof && in->len == 0)
        return NULL;
    /* Jansson rejects malformed UTF-8, so normalize external input before returning it. */
    return utf8_sanitize(in->buf, in->len);
}
