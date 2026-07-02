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
    const int icon_w = 2;
    int budget = cols - icon_w; /* cells left for text after the icon */
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

static void render_row(struct buf *out, const struct picker_state *s, size_t fi, int selected,
                       int cols, int utf8)
{
    const struct picker_item *it = &s->opts->items[s->filtered[fi]];
    int budget = cols - 2; /* leave the 2-cell marker column */
    if (budget < 1)
        budget = 1;

    if (it->disabled) {
        /* Dim, unselectable: no marker arrow and no highlight even if the
         * cursor is parked here (only possible when every row is disabled).
         * The detail column carries the reason. One DIM .. BOLD_OFF spans
         * the whole row so nothing inside re-brightens it. */
        buf_append_str(out, ANSI_DIM "  ");
        struct buf det;
        buf_init(&det);
        int det_w = 0;
        const char *reason = (it->detail && it->detail[0]) ? it->detail : NULL;
        if (reason) {
            int maxd = budget / 2;
            if (maxd < 1)
                maxd = 1;
            append_clip(&det, reason, maxd, &det_w, utf8);
        }
        int lab_budget = budget - (det_w ? det_w + 2 : 0);
        if (lab_budget < 1)
            lab_budget = 1;
        int lab_used = 0;
        append_clip(out, it->label ? it->label : "", lab_budget, &lab_used, utf8);
        if (det_w) {
            buf_append_str(out, "  ");
            buf_append(out, det.data ? det.data : "", det.len);
        }
        buf_free(&det);
        buf_append_str(out, ANSI_BOLD_OFF);
        return;
    }

    if (selected)
        buf_append_str(out, ANSI_BRIGHT_MAGENTA);
    buf_append_str(out, selected ? (utf8 ? "\xe2\x86\x92 " : "> ") : "  "); /* → */
    if (selected)
        buf_append_str(out, ANSI_FG_DEFAULT);

    /* The detail (e.g. a relative time) follows the label after a small gap,
     * staying close to the text rather than flush-right — clip the label so
     * the detail still fits on the row. */
    struct buf det;
    buf_init(&det);
    int det_w = 0;
    const char *detail = (it->detail && it->detail[0]) ? it->detail : NULL;
    if (detail) {
        int maxd = budget / 2;
        if (maxd < 1)
            maxd = 1;
        append_clip(&det, detail, maxd, &det_w, utf8);
    }

    int lab_budget = budget - (det_w ? det_w + 2 : 0);
    if (lab_budget < 1)
        lab_budget = 1;
    int lab_used = 0;
    if (selected)
        buf_append_str(out, ANSI_BOLD);
    append_clip(out, it->label ? it->label : "", lab_budget, &lab_used, utf8);
    if (selected)
        buf_append_str(out, ANSI_BOLD_OFF);

    if (det_w) {
        buf_append_str(out, "  ");
        buf_append_str(out, ANSI_DIM);
        buf_append(out, det.data ? det.data : "", det.len);
        buf_append_str(out, ANSI_BOLD_OFF);
    }
    buf_free(&det);
}

static void paint(struct picker_state *s)
{
    int cols, rows;
    term_size(&cols, &rows);
    (void)rows;
    int utf8 = locale_have_utf8();

    struct buf out, row;
    buf_init(&out);
    buf_init(&row);

    /* The whole frame is one fwrite already; wrap it in synchronized output
     * (DEC 2026) too. Redraw before erasing stale tails so terminals or tmux
     * setups that ignore synchronized output don't show a blank picker between
     * frames. */
    buf_append_str(&out, ANSI_SYNC_BEGIN);

    /* Climb to the top of the prior paint. Stale content is cleared after
     * each redrawn row and below the final row. */
    if (s->painted && s->prev_rows > 1) {
        char up[16];
        snprintf(up, sizeof up, "\x1b[%dA", s->prev_rows - 1);
        buf_append_str(&out, up);
    }
    if (s->painted)
        buf_append(&out, "\r", 1);

    /* Each logical row is built into `row`, then flushed via the clipping
     * backstop, tail-cleared, and joined with a leading CR/LF before all but
     * the first — so the cursor parks on the final row, which the reposition
     * math above and the exit erase both key off. */
    int painted_rows = 0;
#define EMIT_ROW()                                                                                 \
    do {                                                                                           \
        if (painted_rows)                                                                          \
            buf_append_str(&out, "\r\n");                                                          \
        painted_rows++;                                                                            \
        emit_line_clipped(&out, row.data ? row.data : "", row.len, cols, utf8);                    \
        buf_append_str(&out, ANSI_ERASE_LINE);                                                     \
        buf_reset(&row);                                                                           \
    } while (0)

    if (s->opts->title) {
        buf_append_str(&row, ANSI_BOLD);
        buf_append_str(&row, s->opts->title);
        buf_append_str(&row, ANSI_BOLD_OFF);
        EMIT_ROW();
        EMIT_ROW(); /* blank line between the title and the search field */
    }

    render_search(&row, s, cols, utf8);
    EMIT_ROW();

    EMIT_ROW(); /* blank line between the search field and the list */

    if (s->n_filtered == 0) {
        buf_append_str(&row, ANSI_DIM "  (no matches)" ANSI_BOLD_OFF);
        EMIT_ROW();
    } else {
        size_t end = s->top + (size_t)s->viewport;
        if (end > s->n_filtered)
            end = s->n_filtered;
        for (size_t fi = s->top; fi < end; fi++) {
            render_row(&row, s, fi, fi == s->sel, cols, utf8);
            EMIT_ROW();
        }
    }
#undef EMIT_ROW

    buf_append_str(&out, ANSI_ERASE_BELOW);
    buf_append_str(&out, ANSI_SYNC_END);

    buf_free(&row);
    fwrite(out.data ? out.data : "", 1, out.len, stdout);
    fflush(stdout);
    buf_free(&out);

    s->prev_rows = painted_rows;
    s->painted = 1;
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
    (void)cols;
    /* Reserve the chrome rows so the list doesn't run off the bottom:
     * optional title + its trailing blank, the search field + its trailing
     * blank, and one row of breathing space. */
    int reserved = (opts->title ? 2 : 0) + 2 + 1;
    int vp = rows - reserved;
    if (vp < 1)
        vp = 1;
    if (vp > PICKER_MAX_ROWS)
        vp = PICKER_MAX_ROWS;
    s.viewport = vp;

    picker_core_recompute(&s);

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
            /* Only an enabled row is acceptable; a disabled highlight
             * (all rows disabled) does nothing. */
            if (s.n_filtered && picker_core_row_enabled(&s, s.sel)) {
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
