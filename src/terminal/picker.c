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

#include "util.h"
#include "terminal/ansi.h"
#include "terminal/input_core.h"
#include "terminal/theme.h"
#include "terminal/picker_core.h"
#include "terminal/ui.h"
#include "text/utf8.h"

/* Per-byte timeout for reassembling a multi-byte key (escape sequence or
 * UTF-8 glyph). Mirrors the line editor's ESC_TIMEOUT_MS — long enough for
 * a real terminal's burst, short enough that a lone ESC reads as cancel. */
#define ESC_TIMEOUT_MS 50

/* Hard ceiling on visible rows so a tall terminal doesn't paint a wall of
 * list; the window scrolls to keep the selection in view past this. */
#define PICKER_MAX_ROWS 12

/* Ceiling on the desc footer's wrapped lines; longer text gets an ellipsis. */
#define PICKER_FOOTER_LINES 3

/* ---------------- terminal geometry ---------------- */

static void term_size(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *cols = ws.ws_col > 0 ? ws.ws_col : 80;
        *rows = ws.ws_row > 0 ? ws.ws_row : 24;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

/* ---------------- raw mode + byte input ---------------- */

static int raw_on(struct picker_state *s)
{
    if (tcgetattr(STDIN_FILENO, &s->saved) < 0)
        return -1;
    struct termios raw = s->saved;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP | BRKINT);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
        return -1;
    s->raw_active = 1;
    return 0;
}

static void raw_off(struct picker_state *s)
{
    if (!s->raw_active)
        return;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &s->saved);
    s->raw_active = 0;
}

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

static int read_byte_timeout(unsigned char *out, int ms)
{
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    int r;
    do {
        r = poll(&pfd, 1, ms);
    } while (r < 0 && errno == EINTR);
    if (r <= 0)
        return r;
    return read_byte_blocking(out);
}

/* A byte source for input_core_decode_escape that can replay one byte we
 * already consumed (to tell a bare ESC from a real sequence) before
 * falling through to the timed tty read. */
struct seq_reader {
    int pending; /* a byte already read, or -1 */
};

static int seq_byte(void *user)
{
    struct seq_reader *r = user;
    if (r->pending >= 0) {
        int b = r->pending;
        r->pending = -1;
        return b;
    }
    unsigned char b;
    return read_byte_timeout(&b, ESC_TIMEOUT_MS) <= 0 ? -1 : b;
}

/* ---------------- render ---------------- */

/* Width of the leading marker column: "→ " on the highlighted row, spaces
 * elsewhere. The search field's icon is the same width so the query and
 * the row labels align vertically. */
#define MARKER_CELLS 2

/* Append a one-line, display-safe clip of `s` (at most `max_cells` cells)
 * to `out`, stopping at the first newline and substituting non-printable
 * codepoints with '?'. When the source overflows the budget (or has more
 * lines), the tail is replaced by an ellipsis. Writes the cells consumed
 * to *used. Keeps every rendered row exactly one physical row, which is
 * what the reposition math in paint() relies on. */
static void append_clip(struct buf *out, const char *s, int max_cells, int *used, int utf8)
{
    if (max_cells < 1) {
        *used = 0;
        return;
    }
    size_t len = strlen(s);
    size_t line_end = len;
    for (size_t k = 0; k < len; k++) {
        if (s[k] == '\n' || s[k] == '\r') {
            line_end = k;
            break;
        }
    }
    int full = 0;
    for (size_t i = 0; i < line_end;) {
        size_t cons;
        int w = utf8_codepoint_cells(s, line_end, i, &cons);
        full += w < 0 ? 1 : w;
        i += cons ? cons : 1;
    }
    int overflow = (line_end < len) || (full > max_cells);
    int budget = overflow ? max_cells - 1 : max_cells;
    int cells = 0;
    for (size_t i = 0; i < line_end;) {
        size_t cons;
        int w = utf8_codepoint_cells(s, line_end, i, &cons);
        int cw = w < 0 ? 1 : w;
        if (cells + cw > budget)
            break;
        if (w < 0)
            buf_append(out, "?", 1);
        else
            buf_append(out, s + i, cons ? cons : 1);
        cells += cw;
        i += cons ? cons : 1;
    }
    if (overflow) {
        buf_append_str(out, utf8 ? "\xe2\x80\xa6" : ".");
        cells++;
    }
    *used = cells;
}

/* Width of `s` as append_clip would render it without a cell limit. */
static int clip_width(const char *s)
{
    size_t len = strlen(s);
    size_t line_end = len;
    for (size_t k = 0; k < len; k++) {
        if (s[k] == '\n' || s[k] == '\r') {
            line_end = k;
            break;
        }
    }
    int cells = 0;
    for (size_t i = 0; i < line_end;) {
        size_t cons;
        int w = utf8_codepoint_cells(s, line_end, i, &cons);
        cells += w < 0 ? 1 : w;
        i += cons ? cons : 1;
    }
    return cells + (line_end < len ? 1 : 0);
}

/* Return bytes for the next line, preferring a space before `width` cells.
 * `skip` receives the separator bytes to discard. */
static size_t wrap_line(const char *s, int width, size_t *skip)
{
    size_t len = strlen(s);
    size_t i = 0, last_space = 0;
    int cells = 0;
    while (i < len) {
        size_t cons;
        int w = utf8_codepoint_cells(s, len, i, &cons);
        int cw = w < 0 ? 1 : w;
        if (cells + cw > width)
            break;
        if (s[i] == ' ')
            last_space = i;
        cells += cw;
        i += cons ? cons : 1;
    }
    if (i >= len) {
        *skip = 0;
        return len;
    }
    if (last_space > 0) {
        *skip = 1;
        return last_space;
    }
    *skip = 0;
    return i;
}

/* Wrapped footer height, capped at `cap`; 0 for no description. */
static int desc_lines(const char *desc, int width, int cap)
{
    if (!desc || !desc[0])
        return 0;
    const char *p = desc;
    int lines = 0;
    while (*p && lines < cap) {
        size_t skip;
        size_t nb = wrap_line(p, width, &skip);
        p += nb + skip;
        lines++;
    }
    return lines;
}

/* Footer text width after its indent and right margin. Clamped to the content
 * display width so the description prose wraps at the same column as the rest
 * of the app rather than stretching across a wide terminal. */
static int footer_width(int cols)
{
    int dw = display_width();
    if (dw < cols)
        cols = dw;
    int w = cols - MARKER_CELLS - 1;
    return w < 8 ? 8 : w;
}

/* Copy one built row to `out`, passing ANSI escape sequences through at zero
 * width while clipping visible content to `cols` cells. This is the single
 * backstop that guarantees no painted row wraps — a wrapped row would desync
 * paint()'s one-physical-row-per-logical-row reposition/erase math. The
 * per-row append_clip() calls still size each row's text for layout (which
 * part to drop, e.g. keep the count/detail); this just holds the invariant no
 * matter how a row was composed, and neutralizes any stray control byte
 * (including an embedded newline) to one cell. On truncation it appends an
 * ellipsis (when a cell is free) and a reset, so a clipped row can't leave an
 * SGR attribute bleeding into the next line. */
static void emit_line_clipped(struct buf *out, const char *line, size_t len, int cols, int utf8)
{
    int cells = 0;
    size_t i = 0;
    while (i < len) {
        if ((unsigned char)line[i] == 0x1b) {
            /* Copy an escape sequence verbatim at zero width: ESC '[' params,
             * up to and including the final byte (0x40-0x7E). */
            size_t j = i + 1;
            if (j < len && line[j] == '[') {
                j++;
                while (j < len && !(line[j] >= 0x40 && line[j] <= 0x7e))
                    j++;
                if (j < len)
                    j++;
            }
            buf_append(out, line + i, j - i);
            i = j;
            continue;
        }
        size_t cons;
        int w = utf8_codepoint_cells(line, len, i, &cons);
        int cw = w < 0 ? 1 : w;
        if (cells + cw > cols) {
            if (cells < cols)
                buf_append_str(out, utf8 ? "\xe2\x80\xa6" : ".");
            buf_append_str(out, ANSI_RESET);
            return;
        }
        if (w < 0)
            buf_append(out, "?", 1);
        else
            buf_append(out, line + i, cons ? cons : 1);
        cells += cw;
        i += cons ? cons : 1;
    }
}

/* The search field: a dim magnifier glyph, then either a dim "type to
 * search" placeholder (empty) or the typed query followed — close by, after
 * a small gap — by the dim matched/total count. Always two cells of icon so
 * the query lines up under the rows' marker column. */
static void render_search(struct buf *out, const struct picker_state *s, int cols, int utf8)
{
    const char *icon = utf8 ? "\xe2\x8c\x95 " : "/ "; /* ⌕ */
    int budget = cols - MARKER_CELLS;                 /* cells left for text after the icon */
    if (budget < 0)
        budget = 0;

    buf_append_str(out, ANSI_DIM);
    buf_append_str(out, icon);

    if (s->query.len == 0) {
        /* Still dim from the icon — show the placeholder and the total so
         * the list's size is visible before any filtering. Clipped to the
         * remaining width: a wrapped header would throw off paint()'s
         * one-physical-row-per-logical-row repaint/erase math. */
        char total[48];
        snprintf(total, sizeof total, "type to search %zu item%s", s->opts->n,
                 s->opts->n == 1 ? "" : "s");
        int used = 0;
        append_clip(out, total, budget, &used, utf8);
        buf_append_str(out, ANSI_BOLD_OFF);
        return;
    }

    char count[32];
    snprintf(count, sizeof count, "%zu/%zu", s->n_filtered, s->opts->n);
    int count_w = (int)strlen(count);

    /* Keep "<query>  <count>" inside the row: reserve the count (plus its
     * two-space gap) only when at least one query cell would still fit,
     * else drop it and give the whole budget to the query. Either way the
     * visible width stays <= cols, so the row can't wrap. */
    int show_count = budget >= count_w + 2 + 1;
    int qbudget = show_count ? budget - (count_w + 2) : budget;
    if (qbudget < 0)
        qbudget = 0;

    buf_append_str(out, ANSI_BOLD_OFF); /* query in normal intensity */
    int qused = 0;
    append_clip(out, s->query.data, qbudget, &qused, utf8);

    if (show_count) {
        buf_append_str(out, "  ");
        buf_append_str(out, ANSI_DIM);
        buf_append_str(out, count);
        buf_append_str(out, ANSI_BOLD_OFF);
    }
}

/* Render `label [ ✓ current ] [ detail ]`. Detail uses remaining width; if
 * both fields overflow, the label keeps at least half the available row. */
static void render_row(struct buf *out, const struct picker_state *s, size_t fi, int selected,
                       int cols, int utf8)
{
    const struct picker_item *it = &s->opts->items[s->filtered[fi]];
    int row_cells = cols - MARKER_CELLS;
    if (row_cells < 1)
        row_cells = 1;

    if (selected)
        buf_append_str(out, theme_open(THEME_ACCENT));
    buf_append_str(out, selected ? (utf8 ? "\xe2\x86\x92 " : "> ") : "  "); /* → */
    if (selected)
        buf_append_str(out, theme_close(THEME_ACCENT));

    const char *tag = it->current ? (utf8 ? "\xe2\x9c\x93 current" : "* current") : NULL; /* ✓ */
    int tag_cells = tag ? (int)strlen("  * current") : 0; /* gap included */

    const char *sep = it->dim ? (utf8 ? " \xe2\x80\x93 " : " - ") : "  "; /* – */
    int sep_cells = (int)strlen(it->dim ? " - " : "  ");

    const char *label = it->label ? it->label : "";
    int avail = row_cells - tag_cells;
    int label_cells = avail;

    struct buf detail;
    buf_init(&detail);
    int detail_cells = 0;
    if (it->detail && it->detail[0]) {
        int lbl_nat = clip_width(label);
        int lbl_room = avail - sep_cells - clip_width(it->detail);
        label_cells = avail / 2 > lbl_room ? avail / 2 : lbl_room;
        if (lbl_nat < label_cells)
            label_cells = lbl_nat;
        append_clip(&detail, it->detail, avail - label_cells - sep_cells, &detail_cells, utf8);
    }

    if (label_cells < 1)
        label_cells = 1;
    int label_used = 0;
    /* A dim row's label stays dim even under the highlight — the arrow
     * alone marks focus there; bold would contradict the "probably won't
     * work" signal. */
    if (it->dim)
        buf_append_str(out, ANSI_DIM);
    else if (selected)
        buf_append_str(out, ANSI_BOLD);
    append_clip(out, label, label_cells, &label_used, utf8);
    if (it->dim || selected)
        buf_append_str(out, ANSI_BOLD_OFF);

    if (tag) {
        buf_append_str(out, "  ");
        buf_append_str(out, theme_open(THEME_OK));
        buf_append_str(out, tag);
        buf_append_str(out, theme_close(THEME_OK));
    }
    if (detail_cells) {
        buf_append_str(out, ANSI_DIM);
        buf_append_str(out, sep);
        buf_append(out, detail.data ? detail.data : "", detail.len);
        buf_append_str(out, ANSI_BOLD_OFF);
    }
    buf_free(&detail);
}

/* Buffers and geometry for one picker repaint. */
struct frame {
    struct buf out; /* the whole escape-sequence frame, flushed in one write */
    struct buf row; /* scratch for the row currently being built */
    int rows;       /* rows emitted so far (the cursor parks on the last) */
    int cols;
    int utf8;
};

static void frame_init(struct frame *f, int cols, int utf8)
{
    buf_init(&f->out);
    buf_init(&f->row);
    f->rows = 0;
    f->cols = cols;
    f->utf8 = utf8;
}

/* Emit one clipped physical line and reset the row buffer. */
static void frame_emit(struct frame *f)
{
    if (f->rows)
        buf_append_str(&f->out, "\r\n");
    f->rows++;
    emit_line_clipped(&f->out, f->row.data ? f->row.data : "", f->row.len, f->cols, f->utf8);
    buf_append_str(&f->out, ANSI_ERASE_LINE);
    buf_reset(&f->row);
}

static void frame_free(struct frame *f)
{
    buf_free(&f->out);
    buf_free(&f->row);
}

/* Render the selected item's description in a fixed-height footer so the
 * frame does not move as selection changes. */
static void render_footer(struct frame *f, const struct picker_state *s)
{
    if (s->footer_lines <= 0)
        return;
    frame_emit(f); /* blank line between the list and the footer */
    const char *desc = s->n_filtered ? s->opts->items[s->filtered[s->sel]].desc : NULL;
    int width = footer_width(f->cols);
    const char *p = desc && desc[0] ? desc : "";
    for (int line = 0; line < s->footer_lines; line++) {
        if (*p) {
            buf_append_str(&f->row, ANSI_DIM "  ");
            if (line == s->footer_lines - 1) {
                /* Clip the remainder with an ellipsis on the final line. */
                int used = 0;
                append_clip(&f->row, p, width, &used, f->utf8);
                p += strlen(p);
            } else {
                size_t skip;
                size_t nb = wrap_line(p, width, &skip);
                buf_append(&f->row, p, nb);
                p += nb + skip;
            }
            buf_append_str(&f->row, ANSI_BOLD_OFF);
        }
        frame_emit(f); /* empty when the desc ran out — pads to fixed height */
    }
}

static void paint(struct picker_state *s)
{
    int cols, rows;
    term_size(&cols, &rows);
    (void)rows;

    struct frame f;
    frame_init(&f, cols, locale_have_utf8());

    /* The whole frame is one fwrite already; wrap it in synchronized output
     * (DEC 2026) too. Redraw before erasing stale tails so terminals or tmux
     * setups that ignore synchronized output don't show a blank picker between
     * frames. */
    buf_append_str(&f.out, ANSI_SYNC_BEGIN);

    /* Climb to the top of the prior paint. Stale content is cleared after
     * each redrawn row and below the final row. */
    if (s->painted && s->prev_rows > 1) {
        char up[16];
        snprintf(up, sizeof up, "\x1b[%dA", s->prev_rows - 1);
        buf_append_str(&f.out, up);
    }
    if (s->painted)
        buf_append(&f.out, "\r", 1);

    if (s->opts->title) {
        buf_append_str(&f.row, ANSI_BOLD);
        buf_append_str(&f.row, s->opts->title);
        buf_append_str(&f.row, ANSI_BOLD_OFF);
        frame_emit(&f);
        frame_emit(&f); /* blank line between the title and the search field */
    }

    render_search(&f.row, s, f.cols, f.utf8);
    frame_emit(&f);

    frame_emit(&f); /* blank line between the search field and the list */

    if (s->n_filtered == 0) {
        buf_append_str(&f.row, ANSI_DIM "  (no matches)" ANSI_BOLD_OFF);
        frame_emit(&f);
    } else {
        size_t end = s->top + (size_t)s->viewport;
        if (end > s->n_filtered)
            end = s->n_filtered;
        for (size_t fi = s->top; fi < end; fi++) {
            render_row(&f.row, s, fi, fi == s->sel, f.cols, f.utf8);
            frame_emit(&f);
        }
    }

    render_footer(&f, s);

    buf_append_str(&f.out, ANSI_ERASE_BELOW);
    buf_append_str(&f.out, ANSI_SYNC_END);

    fwrite(f.out.data ? f.out.data : "", 1, f.out.len, stdout);
    fflush(stdout);

    s->prev_rows = f.rows;
    s->painted = 1;
    frame_free(&f);
}

/* ---------------- public entry ---------------- */

long picker_run(const struct picker_opts *opts)
{
    if (!opts || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return -1;
    if (opts->n == 0) {
        if (opts->empty_note)
            ui_note("%s", opts->empty_note);
        return -1;
    }

    struct picker_state s;
    memset(&s, 0, sizeof s);
    s.opts = opts;
    s.filtered = xmalloc(opts->n * sizeof(*s.filtered));
    buf_init(&s.query);

    int cols, rows;
    term_size(&cols, &rows);
    /* Keep footer height stable across selections. */
    int fw = footer_width(cols);
    for (size_t i = 0; i < opts->n; i++) {
        int d = desc_lines(opts->items[i].desc, fw, PICKER_FOOTER_LINES);
        if (d > s.footer_lines)
            s.footer_lines = d;
    }
    /* Reserve title, search, spacing, and footer rows from the viewport. */
    int reserved = (opts->title ? 2 : 0) + 2 + 1;
    if (s.footer_lines > 0)
        reserved += s.footer_lines + 1;
    int vp = rows - reserved;
    if (vp < 1)
        vp = 1;
    if (vp > PICKER_MAX_ROWS)
        vp = PICKER_MAX_ROWS;
    s.viewport = vp;

    picker_core_recompute(&s);
    if (opts->initial)
        picker_core_select_item(&s, opts->initial);

    if (raw_on(&s) < 0) {
        free(s.filtered);
        buf_free(&s.query);
        return -1;
    }
    /* The search field is a labeled box, not a cursor position — hide the
     * terminal cursor so it doesn't sit distractingly at the end of a row. */
    fputs(ANSI_CURSOR_HIDE, stdout);
    fflush(stdout);
    paint(&s);

    long result = -1;
    int done = 0;
    while (!done) {
        unsigned char c;
        if (read_byte_blocking(&c) <= 0)
            break; /* EOF — cancel */

        if (c == 0x03 || c == 0x07) /* Ctrl-C / Ctrl-G — cancel */
            break;
        if (c == 0x0d || c == 0x0a) { /* Enter / LF — accept */
            if (s.n_filtered) {
                result = (long)s.filtered[s.sel];
                done = 1;
            }
        } else if (c == 0x7f || c == 0x08) { /* Backspace */
            if (s.query.len) {
                s.query.len = utf8_prev(s.query.data, s.query.len);
                s.query.data[s.query.len] = '\0';
                picker_core_recompute(&s);
            }
        } else if (c == 0x15) { /* Ctrl-U — clear filter */
            if (s.query.len) {
                s.query.len = 0;
                s.query.data[0] = '\0';
                picker_core_recompute(&s);
            }
        } else if (c == 0x0e || c == 0x10) { /* Ctrl-N / Ctrl-P */
            picker_core_move_sel(&s, c == 0x0e ? +1 : -1);
        } else if (c == 0x1b) { /* ESC: bare = cancel, otherwise a key sequence */
            unsigned char nb;
            if (read_byte_timeout(&nb, ESC_TIMEOUT_MS) <= 0)
                break; /* bare ESC — cancel */
            struct seq_reader r = {.pending = nb};
            enum input_action a = input_core_decode_escape(seq_byte, &r);
            if (a == INPUT_ACTION_HISTORY_PREV)
                picker_core_move_sel(&s, -1);
            else if (a == INPUT_ACTION_HISTORY_NEXT)
                picker_core_move_sel(&s, +1);
            else if (a == INPUT_ACTION_LINE_START)
                picker_core_select_first(&s);
            else if (a == INPUT_ACTION_LINE_END)
                picker_core_select_last(&s);
            else if (a == INPUT_ACTION_PAGE_UP)
                picker_core_page_sel(&s, -1);
            else if (a == INPUT_ACTION_PAGE_DOWN)
                picker_core_page_sel(&s, +1);
            /* Any other sequence (unknown key) is ignored — never an
             * accidental cancel. */
        } else if (c >= 0x20) { /* printable — extend the filter */
            int seq = utf8_seq_len(c);
            char bytes[4];
            bytes[0] = (char)c;
            int got = 1;
            for (int i = 1; i < seq; i++) {
                unsigned char b;
                if (read_byte_timeout(&b, ESC_TIMEOUT_MS) <= 0)
                    break;
                bytes[got++] = (char)b;
            }
            buf_append(&s.query, bytes, got);
            picker_core_recompute(&s);
        }
        /* Other control bytes: ignore. */

        if (!done)
            paint(&s);
    }

    /* Erase the picker's painted area; leave the cursor at column 0 of a
     * clean line for the caller's next output, and restore the cursor. */
    if (s.painted && s.prev_rows > 0) {
        if (s.prev_rows > 1)
            printf("\x1b[%dA", s.prev_rows - 1);
        fputs("\r\x1b[J", stdout);
    }
    fputs(ANSI_CURSOR_SHOW, stdout);
    fflush(stdout);
    raw_off(&s);

    free(s.filtered);
    buf_free(&s.query);
    return result;
}
