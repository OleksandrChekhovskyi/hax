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
#include <sys/wait.h>

#include "util.h"
#include "system/fs.h"
#include "system/spawn.h"
#include "terminal/ansi.h"
#include "terminal/input_core.h"
#include "text/utf8.h"
#include "text/utf8_sanitize.h"

#define ESC_TIMEOUT_MS 50

static int tty_cols(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 0;
}

static int editor_cols(void)
{
    int cols = display_width();
    int real = tty_cols();

    if (real > 0 && cols > real)
        cols = real;
    if (cols < 1)
        cols = 1;
    return cols;
}

int input_display_cols(void)
{
    return editor_cols();
}

/* ---------------- raw mode ---------------- */

static void raw_on(struct input *in)
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

    /* Enable bracketed paste so multi-line pastes don't fire keybindings. */
    fputs("\x1b[?2004h", stdout);
    fflush(stdout);
    in->raw_active = 1;
}

static void raw_off(struct input *in)
{
    if (!in->raw_active)
        return;
    fputs("\x1b[?2004l", stdout);
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

/* ---------------- bracketed paste ---------------- */

/* Sink for paste-body bytes — receives the content between the bracketed-
 * paste markers, after CR→LF/NUL normalization, one byte at a time. The
 * main editor feeds it into the edit buffer; the Ctrl-R loop feeds it into
 * the search query. */
typedef void (*paste_sink)(void *user, const char *bytes, size_t n);

/* Read a paste body until we see "\x1b[201~", handing bytes to `sink`.
 * CR is normalized to LF; an LF immediately following a normalized CR
 * is swallowed so Windows-style CRLF pastes become a single newline.
 * NULs are substituted with spaces so they can't truncate downstream
 * NUL-terminated string operations. Held bytes are flushed if they
 * turn out not to be the prefix of the end marker.
 *
 * Bounded against runaway / malicious input: a 5 s idle timeout
 * abandons the paste if bytes stop arriving (a buggy or hostile
 * counterpart can otherwise hold the prompt open indefinitely), and
 * sunk bytes are capped at PASTE_MAX. We keep consuming bytes
 * past the cap so leftover paste body doesn't leak into the next
 * keystroke loop as garbage input. */
static void read_paste(paste_sink sink, void *user)
{
    static const char END[] = "\x1b[201~";
    const size_t ELEN = sizeof(END) - 1;
    const size_t PASTE_MAX = 1u << 20; /* 1 MiB of insertable content */
    const int IDLE_MS = 5000;
    char held[8];
    size_t hlen = 0;
    int swallow_lf = 0;
    size_t inserted = 0;

    for (;;) {
        unsigned char b;
        if (read_byte_timeout(&b, IDLE_MS) <= 0)
            return;
        if (b == '\n' && swallow_lf) {
            swallow_lf = 0;
            continue;
        }
        if (b == '\r') {
            b = '\n';
            swallow_lf = 1;
        } else {
            swallow_lf = 0;
        }
        if (b == '\0')
            b = ' ';
        held[hlen++] = (char)b;

        for (;;) {
            size_t cmp = hlen < ELEN ? hlen : ELEN;
            if (memcmp(held, END, cmp) == 0) {
                if (hlen == ELEN)
                    return; /* full marker matched */
                break;      /* still a possible prefix; keep reading */
            }
            if (inserted < PASTE_MAX) {
                sink(user, &held[0], 1);
                inserted++;
            }
            memmove(held, held + 1, hlen - 1);
            hlen--;
            if (hlen == 0)
                break;
        }
    }
}

static void paste_into_buf(void *user, const char *bytes, size_t n)
{
    input_core_buf_insert((struct input *)user, bytes, n);
}

/* Insert pasted content at the cursor. The start marker "\x1b[200~" has
 * already been consumed by the escape decoder. */
static void handle_paste(struct input *in)
{
    read_paste(paste_into_buf, in);
}

/* ---------------- escape sequence dispatch ---------------- */

/* Adapt read_byte_timeout to the input_byte_reader signature so
 * input_core_decode_escape can drive it without knowing about poll. */
static int byte_reader_tty(void *user)
{
    (void)user;
    unsigned char b;
    int r = read_byte_timeout(&b, ESC_TIMEOUT_MS);
    if (r <= 0)
        return -1;
    return b;
}

/* Apply a decoded action to the edit buffer. Keeping this switch in
 * the IO layer means the decoder stays pure; testing the action
 * dispatch itself is unnecessary because each branch is one line. */
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
        input_core_buf_insert(in, "\n", 1);
        return;
    case INPUT_ACTION_PASTE_BEGIN:
        handle_paste(in);
        return;
    }
}

/* Called after we've consumed an ESC byte. Decodes the rest of the
 * sequence via the pure decoder and applies the resulting action. */
static void handle_escape(struct input *in)
{
    apply_action(in, input_core_decode_escape(byte_reader_tty, NULL));
}

/* ---------------- render / paint ---------------- */

/* Append a CSI sequence like "\x1b[<n><final>" to `f`. */
static void buf_append_csi(struct buf *f, int n, char final)
{
    char tmp[24];
    int len = snprintf(tmp, sizeof(tmp), "\x1b[%d%c", n, final);
    buf_append(f, tmp, (size_t)len);
}

/* Walker callback for the live edit area: appends glyph bytes verbatim
 * and a `\r\n` + indent-spaces at every row break. With OPOST off (raw
 * mode) the terminal won't add the CR for us. The indent target is
 * delivered via ev->col — the walker has resolved it from the
 * cont_indent_col passed in. Output goes into the caller's `struct buf`
 * (passed as `user`) so the whole repaint reaches the tty as one write —
 * see paint(). */
static void paint_emit(const struct input_render_event *ev, void *user)
{
    struct buf *f = user;
    if (ev->kind == INPUT_RENDER_GLYPH) {
        buf_append(f, ev->bytes, ev->n);
    } else {
        buf_append(f, "\r\n", 2);
        for (int k = 0; k < ev->col; k++)
            buf_append(f, " ", 1);
    }
}

/* Repaint the whole edit area. The entire frame — sync-begin, the
 * climb/erase, the prompt, the rendered buffer, the cursor repositioning,
 * sync-end — is assembled in one `struct buf` and emitted with a single
 * fwrite. Two things matter for flicker on a tall prompt: (1) stdout is
 * line-buffered on a tty, so emitting row-by-row would flush once per
 * `\n` and hand the terminal a separate write (and a chance to present a
 * half-drawn frame) for every visual row — batching collapses that to one
 * write; (2) DEC 2026 (ANSI_SYNC_BEGIN/END) tells terminals that support
 * it to present the frame atomically, so the erase-then-redraw can't show
 * an intermediate blank even if the bytes are chunked downstream. */
static void paint(struct input *in)
{
    int prompt_w = input_core_prompt_width(in->prompt);
    int cols = in->term_cols;

    struct buf f;
    buf_init(&f);
    buf_append_str(&f, ANSI_SYNC_BEGIN);

    /* Move to top of last edit area, erase to end of screen. */
    if (in->last_cursor_row > 0)
        buf_append_csi(&f, in->last_cursor_row, 'A');
    buf_append_str(&f, "\r" ANSI_ERASE_BELOW);

    buf_append_str(&f, in->prompt);

    int cont = in->wrap_cont_col0 ? 0 : prompt_w;
    struct input_layout L;
    input_core_render(in->buf, in->len, in->cursor, prompt_w, cont, cols, paint_emit, &f, &L);

    /* From end-of-content position, climb up to cursor row, then right. */
    int up = L.end_row - L.cursor_row;
    if (up > 0)
        buf_append_csi(&f, up, 'A');
    buf_append(&f, "\r", 1);
    if (L.cursor_col > 0)
        buf_append_csi(&f, L.cursor_col, 'C');

    buf_append_str(&f, ANSI_SYNC_END);

    fwrite(f.data, 1, f.len, stdout);
    fflush(stdout);
    buf_free(&f);

    in->last_cursor_row = L.cursor_row;
    in->last_rows = L.total_rows;
}

/* Per-logical-row content width collector. The walker emits a glyph
 * event per cell and a row-break event between rows; tracking the max
 * post-emit col per row gives each row's end column under the OLD
 * width, which is what we need to compute physical-row counts under
 * the new width. */
struct row_widths_state {
    int *widths;
    int cap;
    int n;
    int current; /* highest post-emit col seen so far on current row */
};

static void row_widths_cb(const struct input_render_event *ev, void *user)
{
    struct row_widths_state *s = user;
    if (ev->kind == INPUT_RENDER_GLYPH) {
        int post = ev->col + ev->width;
        if (post > s->current)
            s->current = post;
    } else { /* ROW_BREAK */
        if (s->n < s->cap)
            s->widths[s->n] = s->current;
        s->n++;
        s->current = ev->col; /* new row starts at cont_indent */
    }
}

/* Detect a terminal resize. The painted edit area's manual `\r\n`
 * breaks survive on screen, but each painted row can still char-wrap
 * to multiple physical rows under a narrower width on terminals that
 * reflow on SIGWINCH (xterm, iTerm2, kitty, Alacritty, …). To clear
 * the right region we re-walk the buffer at the *previous* width to
 * recover each logical row's content width, then translate into a
 * physical-row climb under the new width.
 *
 * This assumes the terminal reflows soft-wrapped screen rows on resize,
 * which is the common behavior for modern terminal emulators. Terminals
 * that clamp/truncate instead may leave a stale row behind or briefly
 * over-clear during the next repaint; optimizing the common path keeps
 * resize UX smooth instead of restarting the prompt for every shrink.
 *
 * Called at the top of each loop iteration — *before* applying the
 * next keypress — so last_rows / last_cursor_row still describe
 * what's on screen. Returns 1 if a resize was handled (caller
 * should repaint). */
static int handle_resize(struct input *in)
{
    int new_cols = editor_cols();
    if (new_cols == in->term_cols)
        return 0;
    int old_cols = in->term_cols;
    in->term_cols = new_cols;

    int climb = 0;
    if (in->last_rows > 0 && new_cols > 0) {
        int prompt_w = input_core_prompt_width(in->prompt);
        int cap = in->last_rows + 1; /* +1 for the post-walk final row */
        int *widths = xcalloc((size_t)cap, sizeof(int));
        struct row_widths_state s = {.widths = widths, .cap = cap, .n = 0, .current = prompt_w};
        struct input_layout L;
        int cont = in->wrap_cont_col0 ? 0 : prompt_w;
        input_core_render(in->buf, in->len, in->cursor, prompt_w, cont, old_cols, row_widths_cb, &s,
                          &L);
        if (s.n < s.cap)
            s.widths[s.n] = s.current;
        s.n++;

        /* Physical rows above the cursor's logical row + cursor's
         * offset within its own row under the new width. An empty
         * logical row still occupies one physical row, so floor at 1. */
        for (int i = 0; i < L.cursor_row && i < s.n; i++) {
            int pr = s.widths[i] > 0 ? (s.widths[i] + new_cols - 1) / new_cols : 1;
            climb += pr;
        }
        climb += L.cursor_col / new_cols;
        free(widths);
    }

    if (climb > 0)
        printf("\x1b[%dA", climb);
    fputs("\r\x1b[J", stdout);
    fflush(stdout);
    in->last_cursor_row = 0;
    in->last_rows = 0;
    return 1;
}

/* Move the terminal cursor below the current edit area and start a
 * fresh row. Used on EOF / Ctrl-C / Ctrl-L. */
static void leave_edit_area(struct input *in)
{
    int down = in->last_rows - 1 - in->last_cursor_row;
    if (down > 0)
        printf("\x1b[%dB", down);
    fputs("\r\n", stdout);
    fflush(stdout);
    in->last_cursor_row = 0;
    in->last_rows = 0;
}

/* Walker callback for the committed-user-message render. Appends a
 * bright-magenta `▌ ` stripe at every row break so wrapped continuation
 * rows stay marked, and resets foreground around the strip glyph so
 * the body inherits magenta without escapes nesting. Output is collected
 * into the caller's `struct buf` (passed as `user`). */
static void submitted_emit(const struct input_render_event *ev, void *user)
{
    struct buf *f = user;
    if (ev->kind == INPUT_RENDER_GLYPH) {
        buf_append(f, ev->bytes, ev->n);
    } else {
        buf_append_str(f, ANSI_FG_DEFAULT "\r\n" ANSI_BRIGHT_MAGENTA "▌ ");
    }
}

/* Append the magenta-striped user message to `f`. Shared by the public
 * one-shot renderer and render_submitted so the stripe layout can't drift. */
static void append_user_message(struct buf *f, const char *text, size_t len, int term_cols)
{
    /* Cell width of "▌ ": one box-drawing glyph plus a space. The walker
     * indents continuation rows to this column so wrapped content aligns
     * under the first row's text. */
    const int bar_col = 2;
    buf_append_str(f, ANSI_BRIGHT_MAGENTA "▌ ");
    input_core_render(text, len, /*cursor=*/0, bar_col, bar_col, term_cols, submitted_emit, f,
                      NULL);
    buf_append_str(f, ANSI_FG_DEFAULT "\r\n");
}

void input_render_user_message(const char *text, size_t len, int term_cols)
{
    struct buf f;
    buf_init(&f);
    append_user_message(&f, text, len, term_cols);
    fwrite(f.data, 1, f.len, stdout);
    fflush(stdout);
    buf_free(&f);
}

/* Erase the edit area and re-emit the buffer with a magenta stripe and
 * magenta body so submitted user messages stay clearly marked in the
 * scrollback against agent output. The erase and the re-emit ride one
 * synchronized frame so the prompt is replaced in place without a flash. */
static void render_submitted(struct input *in)
{
    struct buf f;
    buf_init(&f);
    buf_append_str(&f, ANSI_SYNC_BEGIN);
    if (in->last_cursor_row > 0)
        buf_append_csi(&f, in->last_cursor_row, 'A');
    buf_append_str(&f, "\r" ANSI_ERASE_BELOW);
    append_user_message(&f, in->buf, in->len, in->term_cols);
    buf_append_str(&f, ANSI_SYNC_END);
    fwrite(f.data, 1, f.len, stdout);
    fflush(stdout);
    buf_free(&f);

    in->last_cursor_row = 0;
    in->last_rows = 0;
}

/* ---------------- $EDITOR escape ---------------- */

static void open_editor(struct input *in)
{
    /* Erase the current edit area before handing off to the editor so
     * the next paint replaces the prompt in place. For altscreen-using
     * editors (vim, nvim, helix, ...) altscreen restore returns the
     * cursor to this cleared position; non-altscreen editors will
     * leave their output visible above. */
    if (in->last_cursor_row > 0)
        printf("\x1b[%dA", in->last_cursor_row);
    fputs("\r\x1b[J", stdout);
    fflush(stdout);
    in->last_cursor_row = 0;
    in->last_rows = 0;
    raw_off(in);

    char path[] = "/tmp/hax-edit-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        goto reenter;
    if (in->len > 0 && write_all(fd, in->buf, in->len) < 0) {
        close(fd);
        unlink(path);
        goto reenter;
    }
    close(fd);

    const char *editor = getenv("VISUAL");
    if (!editor || !*editor)
        editor = getenv("EDITOR");
    if (!editor || !*editor)
        editor = "vi";

    /* The mkstemp path is single-quoted so paths with metacharacters
     * are safe (mkstemp templates only produce [/A-Za-z0-9_-] anyway).
     * `editor` itself is interpolated raw, on the assumption the user's
     * own env (VISUAL/EDITOR) is trusted — same posture as every other
     * tool that honors $EDITOR.
     *
     * spawn_run, like system(), shields the parent from terminal-
     * generated SIGINT/SIGQUIT during the editor's run, and additionally
     * resets SIGPIPE in the child so editor-internal pipelines (e.g.
     * vim's `:!yes | head`) work the way the user expects. */
    char *cmd = xasprintf("%s '%s'", editor, path);
    int rc = spawn_run(cmd);
    free(cmd);

    /* If the editor exited non-zero (e.g. vim's :cq, or fork/exec
     * failed), treat it as "abort, don't apply the edit". This matches
     * the conventional editor-launched-from-other-tools contract. */
    int aborted = (rc == -1) || (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) || !WIFEXITED(rc);

    size_t n = 0;
    char *content = aborted ? NULL : slurp_file(path, &n);
    unlink(path);
    if (content) {
        /* Substitute embedded NULs with spaces. The rest of the
         * pipeline (xstrdup at submit, history strdup, strlen-based
         * comparisons) treats the buffer as a NUL-terminated string,
         * so a stray NUL would silently truncate everything past it.
         * Substitution preserves byte length for what remains. */
        for (size_t k = 0; k < n; k++) {
            if (content[k] == '\0')
                content[k] = ' ';
        }
        /* Drop one trailing newline — most editors append one automatically. */
        if (n > 0 && content[n - 1] == '\n')
            content[--n] = '\0';
        input_core_buf_set(in, content);
        free(content);
    }

reenter:
    raw_on(in);
    /* Terminal may have been resized while the editor had the screen.
     * SIGWINCH delivered during system() is consumed by the child, so
     * we won't see it here — refresh explicitly before the caller's
     * next paint, otherwise wrap math uses pre-editor width. */
    in->term_cols = editor_cols();
}

/* ---------------- Ctrl-T transcript ---------------- */

/* Same shape as open_editor: erase the edit area, drop raw mode, hand
 * stdout off to the user's callback (which typically popens a pager),
 * then re-enter raw mode and let the caller's next paint redraw the
 * prompt. */
static void show_transcript(struct input *in)
{
    if (!in->transcript_cb)
        return;

    if (in->last_cursor_row > 0)
        printf("\x1b[%dA", in->last_cursor_row);
    fputs("\r\x1b[J", stdout);
    fflush(stdout);
    in->last_cursor_row = 0;
    in->last_rows = 0;
    raw_off(in);

    in->transcript_cb(in->transcript_user);

    raw_on(in);
    /* Pager may have prompted a window resize; refresh before the next
     * paint so wrap math uses the current width. */
    in->term_cols = editor_cols();
}

/* ---------------- reverse / forward incremental search ---------------- */

/* Outcome of the Ctrl-R sub-loop, reported back to the main keystroke
 * loop. ACCEPT leaves the matched (or, on abort, the original) line in the
 * edit buffer and returns to editing; SUBMIT additionally asks the caller
 * to submit it, matching readline's Enter-during-isearch behavior. */
enum rsearch_outcome {
    RSEARCH_ACCEPT,
    RSEARCH_SUBMIT,
};

/* Paste sink for the search query: append printable bytes, dropping ASCII
 * controls and DEL (a pasted newline/tab must not enter the single-line
 * query). UTF-8 lead/continuation bytes (>= 0x80) are kept so multi-byte
 * glyphs survive; any dangerous ones (C1, bidi) are neutralized when the
 * query is sanitized for display — see the prompt build in reverse_search. */
static void paste_into_query(void *user, const char *bytes, size_t n)
{
    struct buf *q = user;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)bytes[i];
        if (b >= 0x20 && b != 0x7f)
            buf_append(q, &bytes[i], 1);
    }
}

/* Recompute the current match after the query changed. An empty query has
 * no match (the pre-search line is shown); otherwise scan from the current
 * match — so refining keeps you on the same entry when it still matches —
 * toward `dir`, marking `failed` when nothing matches. */
static void rsearch_recompute(struct input *in, const struct buf *q, int dir, long *match,
                              int *failed)
{
    if (q->len == 0) {
        *match = -1;
        *failed = 0;
        return;
    }
    long start = (*match >= 0) ? *match : (dir < 0 ? (long)in->hist_n - 1 : 0);
    long m = input_core_history_search(in, q->data, start, dir);
    if (m >= 0) {
        *match = m;
        *failed = 0;
    } else {
        *failed = 1;
    }
}

/* Sanitize the search query for display in the prompt. The prompt is
 * written raw via fputs (unlike the edit buffer, which renders through the
 * substituting walker), so dangerous codepoints must be neutralized here.
 * Substitutes one '?' per ASCII control / DEL / C1 / malformed / dangerous
 * (bidi, invisible) codepoint — the same policy as the buffer walker, keyed
 * on utf8_codepoint_cells returning negative — but passes every other glyph,
 * crucially ordinary spaces, through byte-for-byte: spaces are meaningful in
 * the raw query that drives input_core_history_search, so the prompt must
 * show them verbatim. Returns malloc'd; caller frees. */
static char *sanitize_query_for_display(const char *s)
{
    size_t len = strlen(s);
    struct buf out;
    buf_init(&out);
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) {
            /* ASCII control / DEL — input filters these out, so this is just
             * defensive; one '?' each. */
            buf_append(&out, "?", 1);
            i++;
            continue;
        }
        size_t cons;
        int w = utf8_codepoint_cells(s, len, i, &cons);
        if (w < 0) {
            buf_append(&out, "?", 1); /* one '?' per dangerous codepoint */
            i += cons ? cons : 1;
            continue;
        }
        size_t n = cons ? cons : 1;
        buf_append(&out, s + i, n);
        i += n;
    }
    return buf_steal(&out);
}

/* Clip `s` to at most `avail` display cells, keeping its tail and prefixing
 * a one-cell marker ("…", or "<" without UTF-8) when truncated. Used to keep
 * the search prompt within one row so it can't soft-wrap the terminal —
 * paint()/handle_resize() track only the rows input_core_render() produces
 * and assume the prompt fits on one row. Applied to the query alone (keeping
 * the tail the user is typing) when the chrome fits, or to the whole plain
 * prompt as a last resort on a terminal too narrow for the chrome. The full
 * query still drives the search; only the display is windowed. Returns
 * malloc'd; caller frees. */
static char *clip_query_left(const char *s, int avail, int utf8)
{
    size_t len = strlen(s);
    int total = 0;
    for (size_t i = 0; i < len;) {
        size_t cons;
        int w = utf8_codepoint_cells(s, len, i, &cons);
        total += w < 0 ? 1 : w;
        i += cons ? cons : 1;
    }
    if (avail < 1)
        return xstrdup("");
    if (total <= avail)
        return xstrdup(s);

    const char *mark = utf8 ? "\xe2\x80\xa6" : "<"; /* … */
    int budget = avail - 1;                         /* one cell for the marker */
    size_t keep = len;
    int used = 0;
    for (size_t i = len; i > 0;) {
        size_t prev = utf8_prev(s, i);
        size_t cons;
        int w = utf8_codepoint_cells(s, len, prev, &cons);
        if (w < 0)
            w = 1;
        if (used + w > budget)
            break;
        used += w;
        keep = prev;
        i = prev;
    }
    return xasprintf("%s%s", mark, s + keep);
}

/* Readline-style incremental history search. Entered on Ctrl-R; the prompt
 * is replaced with "reverse-search · query → " and the edit buffer mirrors
 * the most recent history entry containing `query`, with the cursor parked
 * at the match. Keys:
 *   - printable bytes extend the query (UTF-8 reassembled like the main loop)
 *   - Backspace shortens it
 *   - Ctrl-R steps to the next older match, Ctrl-S to the next newer one
 *     (Ctrl-S reaches us because raw mode clears IXON; the prompt flips to
 *     "forward-search")
 *   - Enter accepts the match and submits; LF / ESC (or any escape sequence)
 *     accept it and drop back into editing
 *   - Ctrl-G / Ctrl-C abort, restoring the line as it was on entry
 * During the loop the buffer holds only the painted view (the match, the
 * pre-search line, or empty under "(no match)"); the committed line is
 * rebuilt from `match` at `done` on accept, and abort restores the saved
 * copy. Accepting while nothing matches restores the entry line and does not
 * submit — you only ever accept/submit a match that is actually showing.
 * in->prompt is borrowed for our search prompt and restored on exit. */
static enum rsearch_outcome reverse_search(struct input *in)
{
    char *orig = xstrdup(in->buf);
    size_t orig_cursor = in->cursor;
    size_t orig_hist_pos = in->hist_pos;
    const char *orig_prompt = in->prompt;

    struct buf q;
    buf_init(&q);
    long match = -1;  /* index of current match, or -1 = none */
    int failed = 0;   /* current query matches nothing — painted as "(no match)" */
    int dir = -1;     /* -1 reverse (older), +1 forward (newer) */
    int accepted = 0; /* exiting to editing with the matched entry (not abort) */
    char *prompt_buf = NULL;
    enum rsearch_outcome outcome = RSEARCH_ACCEPT;

    /* The search prefix is wide; wrap a long match's continuation rows to
     * column 0 instead of aligning them under it. */
    in->wrap_cont_col0 = 1;

    for (;;) {
        /* Mirror the current state into the edit buffer for display: the
         * matched entry (cursor parked on the matched span), or the saved
         * line when there's no query yet, or an empty line when the current
         * query matches nothing (the prompt then shows "(no match)"). The
         * line actually committed on accept is rebuilt at `done` from
         * `match`/`orig`, so this is purely what's painted. */
        if (failed) {
            input_core_buf_set(in, "");
        } else if (match >= 0) {
            input_core_buf_set(in, in->hist[match]);
            char *p = q.len > 0 ? strstr(in->buf, q.data) : NULL;
            in->cursor = p ? (size_t)(p - in->buf) : in->len;
        } else {
            input_core_buf_set(in, orig);
            in->cursor = orig_cursor <= in->len ? orig_cursor : in->len;
        }

        /* Modernized chrome: "<reverse|forward>-search · <query> → <result>".
         * Bright magenta for the search chrome — matching the normal "❯"
         * prompt — with the result reset to default; a failed query shows a
         * dim "(no match)" in place of the result. Glyphs fall back to ASCII
         * (" : " / " > ") when the locale isn't UTF-8, like PROMPT_ASCII.
         *
         * The displayed query is sanitized (see sanitize_query_for_display):
         * the prompt is written raw via fputs (unlike the edit buffer, which
         * renders through the substituting walker), so pasted/typed control,
         * DEL, C1 and dangerous bidi/format codepoints must be neutralized
         * here or they could corrupt the terminal. Ordinary spaces are kept
         * verbatim (they matter to the search); the raw query still drives
         * the byte-safe search.
         *
         * The assembled prompt is kept within one row so it can't soft-wrap
         * (paint()/handle_resize() model it as a single row): with room for
         * the chrome, the query is windowed to its tail to fill the leftover
         * width; on a terminal too narrow even for the chrome, the whole
         * plain prompt is left-clipped to fit. input_core_prompt_width strips
         * the CSI sequences, so the wrap and cursor math stay correct. */
        int utf8 = locale_have_utf8();
        const char *label = dir < 0 ? "reverse-search" : "forward-search";
        const char *dot = q.len > 0 ? (utf8 ? " \xc2\xb7 " : " : ") : "";
        const char *arrow = utf8 ? " \xe2\x86\x92 " : " > ";
        char *qsafe = q.len > 0 ? sanitize_query_for_display(q.data) : NULL;

        int budget = in->term_cols - 1;
        if (budget < 1)
            budget = 1;
        /* Fixed chrome: label + " · "/" → " (3 each, dot only with a query) +
         * "(no match)" (10, failed only). */
        int fixed = (int)strlen(label) + (q.len > 0 ? 3 : 0) + 3 + (failed ? 10 : 0);

        free(prompt_buf);
        if (fixed <= budget) {
            char *qdisp = qsafe ? clip_query_left(qsafe, budget - fixed, utf8) : NULL;
            const char *tail = failed ? ANSI_DIM "(no match)" ANSI_BOLD_OFF : "";
            prompt_buf = xasprintf(ANSI_BRIGHT_MAGENTA "%s%s%s%s" ANSI_FG_DEFAULT "%s", label, dot,
                                   qdisp ? qdisp : "", arrow, tail);
            free(qdisp);
        } else {
            char *plain = xasprintf("%s%s%s%s%s", label, dot, qsafe ? qsafe : "", arrow,
                                    failed ? "(no match)" : "");
            char *fit = clip_query_left(plain, budget, utf8);
            prompt_buf = xasprintf(ANSI_BRIGHT_MAGENTA "%s" ANSI_FG_DEFAULT, fit);
            free(plain);
            free(fit);
        }
        free(qsafe);
        in->prompt = prompt_buf;
        paint(in);

        unsigned char c;
        if (read_byte_blocking(&c) <= 0) /* EOF: abort, restore */
            break;

        /* Resize bookkeeping before the next top-of-loop paint, same as
         * the main keystroke loop. */
        handle_resize(in);

        if (c == 0x12 || c == 0x13) { /* Ctrl-R / Ctrl-S — step the search */
            dir = (c == 0x12) ? -1 : 1;
            /* Nothing to step when there's no query yet, or the query
             * matches nothing at all (stays "(no match)"). Otherwise a
             * match is showing (match >= 0); try the next one in `dir`.
             * If exhausted, keep the current match shown — a silent no-op,
             * like Up/Down at the history boundaries — rather than blanking
             * to "(no match)", which would wrongly imply the query stopped
             * matching. So stepping never sets `failed`. */
            if (q.len == 0 || failed)
                continue;
            long m = input_core_history_search(in, q.data, match + dir, dir);
            if (m >= 0)
                match = m;
            continue;
        }
        if (c == 0x7f || c == 0x08) { /* Backspace — shorten the query */
            if (q.len > 0) {
                q.len = utf8_prev(q.data, q.len);
                q.data[q.len] = '\0';
            }
            rsearch_recompute(in, &q, dir, &match, &failed);
            continue;
        }
        if (c == 0x07 || c == 0x03) /* Ctrl-G / Ctrl-C — abort, restore */
            break;
        if (c == 0x0d) { /* Enter — accept the match and submit */
            accepted = 1;
            outcome = RSEARCH_SUBMIT;
            goto done;
        }
        if (c == 0x0a) { /* LF — accept the match, keep editing */
            accepted = 1;
            goto done;
        }
        if (c == 0x1b) {
            /* An escape sequence. A bracketed paste feeds its body into the
             * query (so a paste during search extends it, and — crucially —
             * the paste body and its end marker can't leak into the main
             * keystroke loop where a pasted newline could submit the prompt).
             * Any other sequence (arrow, bare ESC, ...) accepts the match and
             * returns to editing; input_core_decode_escape has already drained
             * its bytes, so nothing leaks. */
            if (input_core_decode_escape(byte_reader_tty, NULL) == INPUT_ACTION_PASTE_BEGIN) {
                read_paste(paste_into_query, &q);
                rsearch_recompute(in, &q, dir, &match, &failed);
                continue;
            }
            accepted = 1;
            goto done;
        }
        if (c >= 0x20) { /* printable — extend the query */
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
            buf_append(&q, bytes, got);
            rsearch_recompute(in, &q, dir, &match, &failed);
            continue;
        }
        /* Other control bytes: ignore, stay in search. */
    }

    /* Abort / EOF path: restore the line exactly as it was on entry. */
    input_core_buf_set(in, orig);
    in->cursor = orig_cursor <= in->len ? orig_cursor : in->len;

done:
    if (accepted) {
        /* Rebuild the committed line from `match` (the display buffer may be
         * the transient empty "(no match)" view). A match is committable only
         * when one is actually showing (!failed) — accepting out of a
         * "(no match)" state must not resurrect a stale, typed-past match, nor
         * submit the original line. So if the current query has no match, we
         * fall back to the line as it was on entry and never submit. */
        if (!failed && match >= 0) {
            input_core_buf_set(in, in->hist[match]);
            char *p = q.len > 0 ? strstr(in->buf, q.data) : NULL;
            in->cursor = p ? (size_t)(p - in->buf) : in->len;
            /* Land history navigation on the matched entry so a subsequent
             * Up/Down steps from there — as if reached by arrowing, matching
             * bash's Ctrl-R. */
            in->hist_pos = (size_t)match;
            /* If the search began on the live draft, preserve it so Down past
             * the newest entry restores what the user was typing. (If they
             * had already arrowed into history, the existing draft is their
             * real in-progress line — leave it untouched.) */
            if (orig_hist_pos == in->hist_n) {
                free(in->draft);
                in->draft = xstrdup(orig);
            }
        } else {
            input_core_buf_set(in, orig);
            in->cursor = orig_cursor <= in->len ? orig_cursor : in->len;
            outcome = RSEARCH_ACCEPT; /* nothing matched — don't submit */
        }
    }
    in->wrap_cont_col0 = 0;
    in->prompt = orig_prompt;
    free(prompt_buf);
    buf_free(&q);
    free(orig);
    return outcome;
}

/* ---------------- non-tty fallback ---------------- */

static char *read_line_canonical(size_t *out_len)
{
    /* No prompt in the non-tty path: stdout may be a pipe or file, in
     * which case ANSI prompt bytes pollute scriptable output, and stdin
     * may be piped with no human to read a prompt anyway. Match
     * libedit's old behavior of staying silent when not interactive. */
    /* Read byte-by-byte rather than fgets+strlen so embedded NULs in
     * piped input survive (fgets terminates at \n but strlen-based
     * append truncates at the first NUL). fgetc returns int and
     * distinguishes 0 from EOF; the length is returned out-of-band so
     * sanitize_utf8 in the caller can turn embedded NULs into U+FFFD
     * without truncating. */
    struct buf b;
    buf_init(&b);
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF)
            break;
        if (ch == '\n') {
            *out_len = b.len;
            return b.data ? buf_steal(&b) : xstrdup("");
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

/* Threshold (multiplier of in-memory cap) for rewriting the on-disk
 * history file at open time. The file grows unboundedly via append; we
 * compact it back to the in-memory snapshot once it gets this far past
 * the cap, so it stays bounded over months of use without paying the
 * rewrite cost on every submit. */
#define HISTORY_FILE_BLOAT_FACTOR 3

/* Hard cap on a single encoded record (bytes between newlines on disk).
 * Drops both at append and at load, so the load path can use a fixed
 * buffer instead of unbounded getline(). 64KB swallows any prompt a
 * human would deliberately submit; the cap exists to defend against
 * file corruption (concurrent-write truncation per the comment above
 * splicing two records together) and against accidentally-edited
 * history files turning into an unbounded allocation at startup. */
#define HISTORY_RECORD_MAX 65536

/* Append one already-encoded entry plus a trailing newline to `path`.
 *
 * Concurrency: the record is built in one buffer and emitted via a
 * single write(2) under O_APPEND. Linux atomically pairs the implicit
 * seek-to-end with the write, so concurrent hax processes don't
 * overlap each other's records — at typical prompt sizes (well under
 * one page) the kernel doesn't return short writes for regular files.
 * A retry loop on a short write would defeat that atomicity (another
 * process could append between our two write calls), so we emit once
 * and accept that a >page record on a stressed system might end up
 * truncated on disk. Errors are swallowed — a failed history write
 * must never disrupt the REPL. */
static void history_file_append(const char *path, const char *line)
{
    /* Refuse a non-regular file (FIFO, device) so a user-planted special
     * node at the history path can't block the REPL on submit. ENOENT
     * is fine — O_CREAT below will make a fresh regular file. */
    if (ensure_regular_file(path) < 0 && errno != ENOENT)
        return;
    char *enc = input_core_history_encode(line);
    size_t n = strlen(enc);
    /* Drop oversized records — see HISTORY_RECORD_MAX. The encoded form
     * can be up to 2x the raw input, so the gate sits here, after
     * encoding. */
    if (n + 1 > HISTORY_RECORD_MAX) {
        free(enc);
        return;
    }
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) {
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

/* Atomically rewrite `path` with the current in-memory history. Uses a
 * sibling tmpfile + rename, so readers and concurrent appenders see
 * either the old or the new file, never a half-written one. The race
 * window with concurrent appenders is small (only invoked at startup
 * after a bloat threshold is crossed) and the worst case is losing a
 * few records that another instance appended during our rewrite. */
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

void input_history_open(struct input *in, const char *path)
{
    if (!path || !*path)
        return;
    /* mkdir -p the parent — first run typically has no $XDG_STATE_HOME
     * tree yet. Failure is non-fatal; the open below will fail and we
     * silently disable persistence. */
    char *dup = xstrdup(path);
    fs_mkdir_p(dirname(dup));
    free(dup);

    /* Refuse a non-regular file at startup — a FIFO would block fopen()
     * indefinitely, freezing the REPL before the first prompt. */
    FILE *f = (ensure_regular_file(path) == 0) ? fopen(path, "r") : NULL;
    size_t loaded = 0;
    if (f) {
        /* Fixed-size buffer instead of getline() — bounds the worst-case
         * allocation if the file contains a corrupt or pathologically
         * long line. Records >= HISTORY_RECORD_MAX bytes are dropped,
         * with the rest of the offending line consumed up to the next
         * newline so we resync on the following record. */
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

    /* Store path before any rewrite so we can no-op on rewrite failure
     * without losing append behavior. */
    free(in->persist_path);
    in->persist_path = xstrdup(path);

    if (loaded > (size_t)INPUT_CORE_HISTORY_MAX * HISTORY_FILE_BLOAT_FACTOR)
        history_file_rewrite(in, path);
}

void input_history_add(struct input *in, const char *line)
{
    if (!line || !*line)
        return;
    if (!input_core_history_add(in, line))
        return;
    if (in->persist_path)
        history_file_append(in->persist_path, line);
}

void input_history_add_session(struct input *in, const char *line)
{
    if (!line || !*line)
        return;
    input_core_history_add(in, line);
}

void input_set_transcript_cb(struct input *in, void (*fn)(void *user), void *user)
{
    in->transcript_cb = fn;
    in->transcript_user = user;
}

void input_history_open_default(struct input *in)
{
    /* Skip persistence in non-interactive sessions. `echo prompt | hax`
     * and other scripted invocations fall through to the canonical
     * non-tty read path (see input_readline), and persisting those
     * one-off lines into the user's recall history is surprising —
     * worse, it can leak secrets piped from a script. Gate on the same
     * condition input_readline uses to choose its read path. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;
    char *path = xdg_hax_state_path("history");
    if (!path)
        return;
    input_history_open(in, path);
    free(path);
}

/* ---------------- public API ---------------- */

char *input_readline(struct input *in, const char *prompt)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        size_t n;
        char *raw = read_line_canonical(&n);
        if (!raw)
            return NULL;
        char *clean = sanitize_utf8(raw, n);
        free(raw);
        return clean;
    }

    /* Reset per-call edit state. History is preserved across calls. */
    in->prompt = prompt;
    in->term_cols = editor_cols();
    in->len = 0;
    in->cursor = 0;
    in->buf[0] = '\0';
    in->hist_pos = in->hist_n;
    free(in->draft);
    in->draft = NULL;
    in->last_cursor_row = 0;
    in->last_rows = 0;

    fflush(stdout);
    raw_on(in);
    paint(in);

    int submit = 0;
    int eof = 0;
    int cancel = 0;

    while (!submit && !eof && !cancel) {
        unsigned char c;
        int r = read_byte_blocking(&c);
        if (r <= 0) {
            eof = 1;
            break;
        }

        /* Resize detection runs after read but before applying the
         * keypress: the buffer still matches what's on screen (the
         * terminal reflowed our last paint while we blocked in read),
         * so the climb-and-clear math has the right inputs. After
         * mutation we'd be working off post-keypress state and the
         * terminal would still show pre-keypress content. */
        if (handle_resize(in))
            paint(in);

        switch (c) {
        case 0x01: /* Ctrl-A */
            in->cursor = input_core_line_start(in);
            break;
        case 0x02: /* Ctrl-B */
            input_core_move_left(in);
            break;
        case 0x03: /* Ctrl-C — cancel: return empty string per readline
                    * convention. The agent loop discards empty results
                    * and re-prompts, leaving the partially-typed line
                    * visible in scrollback. */
            cancel = 1;
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
        case 0x09: /* Tab — insert literal; rendering expands to spaces */
            input_core_buf_insert(in, "\t", 1);
            break;
        case 0x0a: /* LF — Shift+Enter inserts a newline */
            input_core_buf_insert(in, "\n", 1);
            break;
        case 0x0b: /* Ctrl-K */
            input_core_kill_to_eol(in);
            break;
        case 0x0c: /* Ctrl-L — clear screen + repaint */
            fputs("\x1b[2J\x1b[H", stdout);
            in->last_cursor_row = 0;
            in->last_rows = 0;
            break;
        case 0x0d: /* CR — Enter submits, no-op on empty buffer */
            if (in->len > 0)
                submit = 1;
            break;
        case 0x0e: /* Ctrl-N */
            input_core_history_next(in);
            break;
        case 0x10: /* Ctrl-P */
            input_core_history_prev(in);
            break;
        case 0x12: /* Ctrl-R — incremental reverse history search */
            if (reverse_search(in) == RSEARCH_SUBMIT && in->len > 0)
                submit = 1;
            break;
        case 0x14: /* Ctrl-T — open transcript pager (no-op if unset) */
            show_transcript(in);
            break;
        case 0x15: /* Ctrl-U */
            input_core_kill_to_bol(in);
            break;
        case 0x17: /* Ctrl-W */
            input_core_kill_word_back(in);
            break;
        case 0x1a: /* Ctrl-Z — suspend (raw mode disables ISIG, so the
                    * tty driver won't generate SIGTSTP for us). Drop
                    * out of raw mode, leave the partial line visible
                    * in scrollback, and raise the signal ourselves. */
            leave_edit_area(in);
            raw_off(in);
            raise(SIGTSTP);
            /* Resumed. The terminal may have been resized while we
             * were stopped; paint state was cleared by leave_edit_area
             * so the next paint draws fresh. */
            raw_on(in);
            in->term_cols = editor_cols();
            break;
        case 0x1b: /* ESC — start of escape sequence */
            handle_escape(in);
            break;
        default:
            if (c >= 0x20) {
                /* Read remaining bytes of the UTF-8 sequence with the
                 * same timeout we use for ESC: a stray malformed leader
                 * (Meta-key from a misconfigured terminal, serial-line
                 * glitch, paste outside bracketed-paste) would otherwise
                 * block here, and ISIG is off so Ctrl-C/D would be
                 * consumed as continuation bytes instead of working.
                 * On timeout we insert what we have; render-time
                 * substitution renders the partial sequence as `?`. */
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
                input_core_buf_insert(in, bytes, got);
            }
            /* otherwise ignore (e.g. Ctrl-J — unhandled) */
            break;
        }

        if (!eof)
            paint(in);
    }

    if (submit && in->len > 0)
        render_submitted(in);
    else
        leave_edit_area(in);
    raw_off(in);

    if (eof && in->len == 0)
        return NULL;
    if (cancel)
        return xstrdup("");
    /* Sanitize before handing off: paste / $EDITOR content may contain
     * malformed UTF-8 (binary fragments, truncated multi-byte) that
     * downstream JSON builders (jansson) reject, silently dropping the
     * user's message. Replace invalid sequences with U+FFFD. */
    return sanitize_utf8(in->buf, in->len);
}
