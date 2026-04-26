/* SPDX-License-Identifier: MIT */
#include "markdown.h"

#include <stdlib.h>
#include <string.h>

#include "ansi.h"
#include "util.h"

/* Sanity bound on the deferred-tail size. Inline markers fit in a few bytes;
 * the only thing that can grow large is a partial fence opener whose info
 * string spans multiple deltas. Realistic openers are well under 1 KiB even
 * with attributes; this cap prevents runaway growth on malformed input. If
 * exceeded, the excess is flushed as plain text. */
#define TAIL_MAX 8192

struct md_renderer {
    md_emit_fn emit_cb;
    void *user;

    /* Bytes deferred from a previous feed because they could form a
     * multi-byte marker that needs lookahead. Grows dynamically so a
     * long fence opener line that spans deltas (e.g. `` ```python
     * title="..." ``) doesn't get its leading backticks flushed as
     * literal text and unbalance subsequent fence parsing. */
    struct buf tail;

    /* Last source byte processed in any prior feed — needed for the
     * intraword check on a marker at position 0 of the current work
     * buffer (e.g. feed("foo") then feed("_bar_") must NOT open italic
     * because the `_` has `o` to its left in the source stream). */
    char prev_byte;

    int at_line_start;

    /* Number of backticks in the active fence's opener — closer must have
     * at least this many, with no info string after, per CommonMark. Lets
     * a ```markdown demo containing inner ```python lines stay open until
     * a *bare* ``` line. */
    size_t fence_open_count;

    /* Style flags. All independent — each maps to a distinct SGR group, so
     * e.g. inline code (cyan) inside bold leaves bold intact when the code
     * span closes. The one exception is heading-vs-inline-bold which both
     * use the same SGR; close_bold re-emits ANSI_BOLD if in_heading is
     * still set so the rest of the heading line stays bold. */
    int in_heading;
    int in_code_fence;
    int in_inline_code;
    int in_bold;
    int in_italic;
};

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void emit_text(struct md_renderer *m, const char *s, size_t n)
{
    m->emit_cb(s, n, 0, m->user);
}

/* Emit an ANSI escape (zero-width). The is_raw=1 flag tells consumers
 * routed through trail/held bookkeeping (like disp_write) to skip it,
 * so a closer after a buffered `\n` doesn't trash the trail counter. */
static void emit_raw(struct md_renderer *m, const char *s)
{
    m->emit_cb(s, strlen(s), 1, m->user);
}

static void open_bold(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD);
    m->in_bold = 1;
}

static void close_bold(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD_OFF);
    m->in_bold = 0;
    if (m->in_heading)
        emit_raw(m, ANSI_BOLD);
}

static void open_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC);
    m->in_italic = 1;
}

static void close_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC_OFF);
    m->in_italic = 0;
}

static void open_inline_code(struct md_renderer *m)
{
    emit_raw(m, ANSI_CYAN);
    m->in_inline_code = 1;
}

static void close_inline_code(struct md_renderer *m)
{
    emit_raw(m, ANSI_FG_DEFAULT);
    m->in_inline_code = 0;
}

static void open_code_fence(struct md_renderer *m)
{
    emit_raw(m, ANSI_DIM);
    m->in_code_fence = 1;
}

static void close_code_fence(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD_OFF);
    m->in_code_fence = 0;
}

static void open_heading(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD);
    m->in_heading = 1;
}

static void close_heading(struct md_renderer *m)
{
    emit_raw(m, ANSI_BOLD_OFF);
    m->in_heading = 0;
}

/* ---------- per-state byte dispatch ----------
 *
 * md_feed splits its work into four handlers, each owning one slice of
 * the state machine. Each returns one of three outcomes:
 *
 *   STEP_ADVANCED  — handler consumed bytes and advanced *i.
 *   STEP_DEFER     — not enough bytes to decide; loop breaks and the
 *                    unprocessed remainder is saved as tail.
 *   STEP_PASS      — handler didn't apply; try the next one. Models the
 *                    fall-throughs (inline-code closing on \n, line-start
 *                    with no special pattern matching).
 */

enum step_result {
    STEP_ADVANCED,
    STEP_DEFER,
    STEP_PASS,
};

/* Inside a code fence: pass through verbatim. The only thing we look for
 * is a closing fence at line start — backticks >= opener count, followed
 * by only optional whitespace and \n (no info string per CommonMark).
 * The marker line is consumed entirely so the dim region surrounds only
 * the content lines. */
static enum step_result step_in_code_fence(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];

    if (m->at_line_start && c == '`') {
        size_t cnt = 0;
        while (*i + cnt < w->len && w->data[*i + cnt] == '`')
            cnt++;
        if (*i + cnt >= w->len)
            return STEP_DEFER;
        if (cnt >= m->fence_open_count) {
            /* Skip optional trailing whitespace; \r is included
             * so a CRLF-terminated closer (`` ```\r\n ``) is
             * recognized — without it the scan would stop at \r
             * and treat the line as content. */
            size_t scan = *i + cnt;
            while (scan < w->len && w->data[scan] != '\n' &&
                   (w->data[scan] == ' ' || w->data[scan] == '\t' || w->data[scan] == '\r'))
                scan++;
            if (scan >= w->len)
                return STEP_DEFER;
            if (w->data[scan] == '\n') {
                close_code_fence(m);
                m->fence_open_count = 0;
                *i = scan + 1;
                m->at_line_start = 1;
                return STEP_ADVANCED;
            }
            /* Non-whitespace before \n — info-string-like content
             * (e.g. ```python inside a markdown demo). Fall through. */
        }
        /* Too few backticks or invalid trailing — fall through. */
    }
    emit_text(m, &c, 1);
    m->at_line_start = (c == '\n');
    (*i)++;
    return STEP_ADVANCED;
}

/* Inside an inline code span: verbatim until the closing backtick. A \n
 * inside an inline code span shouldn't happen in our prose, but if it
 * does we close the span and PASS so the inline handler emits the \n
 * with normal at_line_start bookkeeping. */
static enum step_result step_in_inline_code(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];
    if (c == '`') {
        close_inline_code(m);
        (*i)++;
        return STEP_ADVANCED;
    }
    if (c == '\n') {
        close_inline_code(m);
        return STEP_PASS;
    }
    emit_text(m, &c, 1);
    (*i)++;
    return STEP_ADVANCED;
}

/* At line start: try the line-level patterns (fence open, heading, list
 * marker). Returns PASS for anything that doesn't match — the caller
 * clears at_line_start and falls through to inline processing. */
static enum step_result step_line_start(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];
    size_t remaining = w->len - *i;

    if (c == '`') {
        /* Count run length so we can support 4+ backtick fences (used
         * to wrap content that itself contains ``` runs). */
        size_t cnt = 0;
        while (*i + cnt < w->len && w->data[*i + cnt] == '`')
            cnt++;
        if (*i + cnt >= w->len)
            return STEP_DEFER;
        if (cnt >= 3) {
            /* Need the rest of the opener line for the info
             * string. If \n isn't here yet, defer. */
            size_t scan = *i + cnt;
            while (scan < w->len && w->data[scan] != '\n')
                scan++;
            if (scan >= w->len)
                return STEP_DEFER;
            open_code_fence(m);
            m->fence_open_count = cnt;
            /* First whitespace-separated token of the info string
             * is the language label. \r is treated as whitespace
             * so a CRLF-terminated opener doesn't leak \r into
             * the rendered label. */
            size_t info = *i + cnt;
            while (info < scan &&
                   (w->data[info] == ' ' || w->data[info] == '\t' || w->data[info] == '\r'))
                info++;
            size_t lang_end = info;
            while (lang_end < scan && w->data[lang_end] != ' ' && w->data[lang_end] != '\t' &&
                   w->data[lang_end] != '\r')
                lang_end++;
            if (lang_end > info) {
                /* Dim is already on from open_code_fence; layer cyan
                 * on top so the label reads as dim cyan, distinct
                 * from the dim-default code body that follows. */
                emit_raw(m, ANSI_CYAN);
                emit_text(m, w->data + info, lang_end - info);
                emit_raw(m, ANSI_FG_DEFAULT);
                emit_text(m, "\n", 1);
            }
            *i = scan + 1; /* past the opener line's \n */
            m->at_line_start = 1;
            return STEP_ADVANCED;
        }
        /* cnt < 3 — not a fence; fall through. */
    }

    if (c == '#') {
        size_t h = 0;
        while (h < 4 && *i + h < w->len && w->data[*i + h] == '#')
            h++;
        if (h < 4) {
            if (*i + h >= w->len)
                return STEP_DEFER;
            if (w->data[*i + h] == ' ' && h >= 1 && h <= 3) {
                open_heading(m);
                *i += h + 1;
                m->at_line_start = 0;
                return STEP_ADVANCED;
            }
        }
        /* 4+ hashes or non-heading sequence — fall through. */
    }

    /* List marker `* ` (asterisk + space). `- ` and `1. ` don't conflict
     * with any inline marker, so they pass through as plain text without
     * special handling. */
    if (c == '*') {
        if (remaining < 2)
            return STEP_DEFER;
        if (w->data[*i + 1] == ' ') {
            emit_text(m, "*", 1);
            m->at_line_start = 0;
            (*i)++;
            return STEP_ADVANCED;
        }
        /* `**...` or `*x` — fall through. */
    }

    return STEP_PASS;
}

/* Helper: emphasis marker is left-flanking (can open) when the byte to
 * the left is non-alphanumeric AND the byte to the right is non-space.
 * Catches the two common false positives — intraword markers (`5*3*7`,
 * `compile_commands.json`) and whitespace-flanked markers (`5 * 3`,
 * indented `  * item` list markers) — without paying for full CommonMark
 * delimiter-run rules. */
static int can_open_emphasis(char left, char right)
{
    return !is_alnum(left) && !is_space(right);
}

/* Inline byte dispatch: \n closes any open per-line styles and resets
 * at_line_start; ` opens inline code; *, **, _ open or close emphasis
 * with the left/right-flanking heuristic. Any other byte emits as text. */
static enum step_result step_inline(struct md_renderer *m, struct buf *w, size_t *i)
{
    char c = w->data[*i];
    size_t remaining = w->len - *i;

    if (c == '\n') {
        if (m->in_heading)
            close_heading(m);
        if (m->in_bold)
            close_bold(m);
        if (m->in_italic)
            close_italic(m);
        emit_text(m, "\n", 1);
        m->at_line_start = 1;
        (*i)++;
        return STEP_ADVANCED;
    }

    if (c == '`') {
        open_inline_code(m);
        (*i)++;
        return STEP_ADVANCED;
    }

    if (c == '*') {
        if (remaining >= 2 && w->data[*i + 1] == '*') {
            if (m->in_bold) {
                close_bold(m);
                *i += 2;
                return STEP_ADVANCED;
            }
            /* Need the byte after `**` to check the right side. */
            if (remaining < 3)
                return STEP_DEFER;
            char l = *i > 0 ? w->data[*i - 1] : m->prev_byte;
            char r = w->data[*i + 2];
            if (can_open_emphasis(l, r)) {
                open_bold(m);
                *i += 2;
                return STEP_ADVANCED;
            }
            emit_text(m, "**", 2);
            *i += 2;
            return STEP_ADVANCED;
        }
        if (remaining < 2)
            return STEP_DEFER;
        if (m->in_italic) {
            close_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        char l = *i > 0 ? w->data[*i - 1] : m->prev_byte;
        char r = w->data[*i + 1];
        if (can_open_emphasis(l, r)) {
            open_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        emit_text(m, &c, 1);
        (*i)++;
        return STEP_ADVANCED;
    }

    if (c == '_') {
        if (m->in_italic) {
            close_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        if (remaining < 2)
            return STEP_DEFER;
        char l = *i > 0 ? w->data[*i - 1] : m->prev_byte;
        char r = w->data[*i + 1];
        if (can_open_emphasis(l, r)) {
            open_italic(m);
            (*i)++;
            return STEP_ADVANCED;
        }
        emit_text(m, &c, 1);
        (*i)++;
        return STEP_ADVANCED;
    }

    emit_text(m, &c, 1);
    (*i)++;
    return STEP_ADVANCED;
}

struct md_renderer *md_new(md_emit_fn emit_cb, void *user)
{
    struct md_renderer *m = xcalloc(1, sizeof(*m));
    m->emit_cb = emit_cb;
    m->user = user;
    m->at_line_start = 1;
    return m;
}

void md_reset(struct md_renderer *m)
{
    buf_reset(&m->tail);
    m->prev_byte = 0;
    m->at_line_start = 1;
    m->fence_open_count = 0;
    m->in_heading = 0;
    m->in_code_fence = 0;
    m->in_inline_code = 0;
    m->in_bold = 0;
    m->in_italic = 0;
}

void md_free(struct md_renderer *m)
{
    if (!m)
        return;
    buf_free(&m->tail);
    free(m);
}

void md_feed(struct md_renderer *m, const char *s, size_t n)
{
    /* Combine pending tail with new input into a working buffer so the
     * walker sees one contiguous stream. */
    struct buf w;
    buf_init(&w);
    if (m->tail.len) {
        buf_append(&w, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }
    buf_append(&w, s, n);

    size_t i = 0;
    while (i < w.len) {
        enum step_result step;

        if (m->in_code_fence) {
            step = step_in_code_fence(m, &w, &i);
            if (step == STEP_DEFER)
                break;
            continue;
        }

        if (m->in_inline_code) {
            step = step_in_inline_code(m, &w, &i);
            if (step == STEP_DEFER)
                break;
            if (step == STEP_ADVANCED)
                continue;
            /* STEP_PASS — \n inside inline code; let inline
             * processing handle the actual newline emit. */
        }

        if (m->at_line_start) {
            step = step_line_start(m, &w, &i);
            if (step == STEP_DEFER)
                break;
            if (step == STEP_ADVANCED)
                continue;
            /* STEP_PASS — no line-start pattern matched; clear
             * at_line_start and fall through to inline. */
            m->at_line_start = 0;
        }

        step = step_inline(m, &w, &i);
        if (step == STEP_DEFER)
            break;
        /* step_inline always advances or defers — never PASSes. */
    }

    /* Save unprocessed remainder as tail. The buf grows dynamically so a
     * long fence opener line that spans deltas is buffered intact. The
     * sanity cap prevents runaway growth on malformed input — the leading
     * excess is flushed as plain text rather than held forever. */
    size_t rem = w.len - i;
    if (rem > TAIL_MAX) {
        emit_text(m, w.data + i, rem - TAIL_MAX);
        i += rem - TAIL_MAX;
        rem = TAIL_MAX;
    }
    if (rem > 0)
        buf_append(&m->tail, w.data + i, rem);

    /* Remember the last source byte we consumed so the next feed's
     * intraword check at i=0 sees the correct left neighbor. */
    if (i > 0)
        m->prev_byte = w.data[i - 1];

    buf_free(&w);
}

void md_flush(struct md_renderer *m)
{
    /* End-of-stream interpretation of any pending tail: the streaming loop
     * deferred these bytes because it needed lookahead to decide. At flush
     * the lookahead is "end of stream", which counts as non-alphanumeric
     * for marker validity — so a trailing closer like ``` (no newline),
     * `*` after italic, or `**` after bold should match cleanly instead of
     * being emitted as literal text. */

    if (m->in_code_fence && m->tail.len > 0) {
        size_t cnt = 0;
        while (cnt < m->tail.len && m->tail.data[cnt] == '`')
            cnt++;
        if (cnt >= m->fence_open_count) {
            int valid = 1;
            for (size_t i = cnt; i < m->tail.len; i++) {
                char tc = m->tail.data[i];
                if (tc != ' ' && tc != '\t' && tc != '\r') {
                    valid = 0;
                    break;
                }
            }
            if (valid) {
                close_code_fence(m);
                m->fence_open_count = 0;
                buf_reset(&m->tail);
            }
        }
    }

    if (m->tail.len == 2 && m->in_bold && m->tail.data[0] == '*' && m->tail.data[1] == '*') {
        close_bold(m);
        buf_reset(&m->tail);
    }

    if (m->tail.len == 1 && m->in_italic && (m->tail.data[0] == '*' || m->tail.data[0] == '_')) {
        close_italic(m);
        buf_reset(&m->tail);
    }

    /* Anything still in tail is a marker that never matched — emit literally. */
    if (m->tail.len) {
        emit_text(m, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }

    /* Close any styles still open so the terminal isn't left in a
     * styled state (e.g. an unmatched `**bold` opener). */
    if (m->in_inline_code)
        close_inline_code(m);
    if (m->in_code_fence)
        close_code_fence(m);
    if (m->in_heading)
        close_heading(m);
    if (m->in_bold)
        close_bold(m);
    if (m->in_italic)
        close_italic(m);
}
