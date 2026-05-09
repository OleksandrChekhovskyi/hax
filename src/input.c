/* SPDX-License-Identifier: MIT */
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "ansi.h"
#include "fs.h"
#include "input_core.h"
#include "spawn.h"
#include "utf8.h"
#include "utf8_sanitize.h"
#include "util.h"

#define ESC_TIMEOUT_MS 50
#define DEFAULT_COLS   80

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

static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return DEFAULT_COLS;
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

/* Read a paste body until we see "\x1b[201~", inserting bytes verbatim.
 * CR is normalized to LF; an LF immediately following a normalized CR
 * is swallowed so Windows-style CRLF pastes become a single newline.
 * NULs are substituted with spaces so they can't truncate downstream
 * NUL-terminated string operations. Held bytes are flushed if they
 * turn out not to be the prefix of the end marker.
 *
 * Bounded against runaway / malicious input: a 5 s idle timeout
 * abandons the paste if bytes stop arriving (a buggy or hostile
 * counterpart can otherwise hold the prompt open indefinitely), and
 * inserted bytes are capped at PASTE_MAX. We keep consuming bytes
 * past the cap so leftover paste body doesn't leak into the next
 * keystroke loop as garbage input. */
static void handle_paste(struct input *in)
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
                input_core_buf_insert(in, &held[0], 1);
                inserted++;
            }
            memmove(held, held + 1, hlen - 1);
            hlen--;
            if (hlen == 0)
                break;
        }
    }
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

/* Emit a span of buffer bytes (no embedded '\n' — caller splits) safely:
 *  - Tabs expand to spaces using the same math as input_core_compute_layout
 *    so layout and rendering can't disagree about where the cursor lands.
 *  - ASCII control bytes (and 0x7f) become '?' so paste/editor content
 *    can't inject terminal escape sequences.
 *  - Printable codepoints (incl. UTF-8 multi-byte) pass through verbatim.
 *
 * `start_col` is the column of the cursor on entry. `cols` is the
 * terminal width (0 = no wrap). Returns the column the cursor lands at
 * after the span. */
static int emit_safe_span(const char *buf, size_t len, int start_col, int cols)
{
    int col = start_col;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\t') {
            int w = INPUT_CORE_TAB_WIDTH;
            if (cols > 0 && col + w > cols) {
                fputs("\r\n", stdout);
                col = 0;
            }
            for (int k = 0; k < w; k++)
                fputc(' ', stdout);
            col += w;
            i++;
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            fputc('?', stdout);
            col += 1;
            i++;
            continue;
        }
        size_t consumed;
        int w = utf8_codepoint_cells(buf, len, i, &consumed);
        if (w < 0) {
            /* Non-printable codepoint: C1 controls (raw 0x80-0x9f or
             * UTF-8 U+0080..U+009F), DEL, format chars, malformed
             * UTF-8. Some terminals interpret these as escape
             * sequences even in UTF-8 mode; substitute. */
            fputc('?', stdout);
            col += 1;
            i += consumed ? consumed : 1;
            continue;
        }
        /* Mirror terminal soft-wrap in our col tracking: when col + w
         * would overflow, the terminal wraps the codepoint to col 0 of
         * the next row before printing it. */
        if (cols > 0 && w > 0 && col + w > cols)
            col = 0;
        fwrite(buf + i, 1, consumed, stdout);
        col += w;
        i += consumed ? consumed : 1;
    }
    return col;
}

/* Emit buffer bytes, translating each '\n' to "\r\n" + `indent` spaces
 * so continuation lines align with the first line's content. We run
 * with OPOST off, so the terminal won't add the CR for us. */
static void emit_buffer(const char *buf, size_t len, int start_col, int indent, int cols)
{
    int col = start_col;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (i > start)
                col = emit_safe_span(buf + start, i - start, col, cols);
            fputs("\r\n", stdout);
            for (int k = 0; k < indent; k++)
                fputc(' ', stdout);
            col = indent;
            start = i + 1;
        }
    }
    if (start < len)
        emit_safe_span(buf + start, len - start, col, cols);
}

static void paint(struct input *in)
{
    int prompt_w = input_core_prompt_width(in->prompt);
    int cols = in->term_cols;

    /* Move to top of last edit area, erase to end of screen. */
    if (in->last_cursor_row > 0)
        printf("\x1b[%dA", in->last_cursor_row);
    fputs("\r\x1b[J", stdout);

    fputs(in->prompt, stdout);
    emit_buffer(in->buf, in->len, prompt_w, prompt_w, cols);

    struct input_layout L;
    input_core_compute_layout(in->buf, in->len, in->cursor, prompt_w, cols, &L);

    /* From end-of-content position, climb up to cursor row, then right. */
    int up = L.end_row - L.cursor_row;
    if (up > 0)
        printf("\x1b[%dA", up);
    putchar('\r');
    if (L.cursor_col > 0)
        printf("\x1b[%dC", L.cursor_col);
    fflush(stdout);

    in->last_cursor_row = L.cursor_row;
    in->last_rows = L.total_rows;
}

/* Detect a terminal resize. The terminal reflowed our prior paint to
 * the new width and the cursor stayed at the same logical position in
 * the buffer (the common reflow behavior across modern terminals).
 * Recompute layout in the new width to find how far up the prompt row
 * is, climb there, and erase the reflowed area; the next paint then
 * draws cleanly. Anything above the prompt row is untouched, so
 * conversation history is preserved.
 *
 * Called at the top of each loop iteration — *before* applying the
 * next keypress — so the buffer state still matches what's on screen.
 * Returns 1 if a resize was handled (caller should repaint). */
static int handle_resize(struct input *in)
{
    int cols = term_cols();
    if (cols == in->term_cols)
        return 0;
    in->term_cols = cols;
    int prompt_w = input_core_prompt_width(in->prompt);
    struct input_layout L;
    input_core_compute_layout(in->buf, in->len, in->cursor, prompt_w, cols, &L);
    if (L.cursor_row > 0)
        printf("\x1b[%dA", L.cursor_row);
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

/* Erase the edit area and re-emit the buffer with a magenta stripe and
 * magenta text body, so submitted user messages stay clearly marked in
 * the scrollback against agent output. The body color is intentional —
 * just a stripe wasn't visually distinct enough during scrollback
 * review. Stripe width matches the prompt (2 cols) so content stays
 * aligned. */
static void render_submitted(struct input *in)
{
    if (in->last_cursor_row > 0)
        printf("\x1b[%dA", in->last_cursor_row);
    fputs("\r\x1b[J", stdout);

    const char *bar = ANSI_BRIGHT_MAGENTA "▌ ";
    const int bar_col = 2; /* visual width of "▌ " */
    int cols = in->term_cols;
    fputs(bar, stdout);
    int col = bar_col;
    size_t start = 0;
    for (size_t i = 0; i < in->len; i++) {
        if (in->buf[i] == '\n') {
            if (i > start)
                col = emit_safe_span(in->buf + start, i - start, col, cols);
            fputs(ANSI_FG_DEFAULT "\r\n", stdout);
            fputs(bar, stdout);
            col = bar_col;
            start = i + 1;
        }
    }
    if (start < in->len)
        emit_safe_span(in->buf + start, in->len - start, col, cols);
    fputs(ANSI_FG_DEFAULT "\r\n", stdout);
    fflush(stdout);
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
    in->term_cols = term_cols();
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
    in->term_cols = term_cols();
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
    in->term_cols = term_cols();
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
            in->term_cols = term_cols();
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
