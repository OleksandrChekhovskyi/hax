/* SPDX-License-Identifier: MIT */
#include "render/markdown.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "render/markdown_scan.h"
#include "render/markdown_table.h"
#include "render/markdown_wrap.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Inline markers are short, but a split fence info line can grow indefinitely on malformed
 * input. Keep a bounded lookahead tail and emit excess bytes literally. */
#define TAIL_MAX 8192

struct md_renderer {
    md_emit_fn emit_cb;
    void *user;

    /* Deferred lookahead, including fence opener lines split across feeds. */
    struct buf tail;

    /* Last consumed source byte, retained for emphasis checks across feeds. */
    char prev_byte;

    int at_line_start;

    /* Trailing source spaces across feeds. Delimiters clear the count so spaces before a
     * marker cannot become hard-break spaces. */
    int trailing_spaces;

    /* Active fence width; a closer needs at least this many backticks and no info string. */
    size_t fence_open_count;

    /* Style flags track parser state independently; nested bold and inline code can still alter
     * SGR attributes owned by a heading, so their closers restore its theme. */
    int in_heading;
    int in_code_fence;
    int in_inline_code;
    int in_bold;
    int in_italic;

    /* Disables SGR output without disabling style-state tracking. */
    int styled;

    /* Block-isolated lines force a hard trailing newline instead of joining with prose. */
    int cur_line_is_block;

    /* Collapses prose blank-line runs; fence and table interiors bypass it. */
    int at_blank;

    /* Drops list-marker padding that continues into the next feed. */
    int skip_pad;

    struct md_wrap wrap;

    struct md_table table;

    /* Table cells parse inline Markdown without recognizing block constructs. */
    int inline_only;

    /* Prevent inner emphasis from canceling a table cell's outer bold style. */
    int suppress_bold;
};

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static struct md_wrap_context wrap_context(const struct md_renderer *m)
{
    return (struct md_wrap_context){
        .emit = m->emit_cb,
        .user = m->user,
        .styled = m->styled,
        .in_bold = m->in_bold,
        .in_italic = m->in_italic,
        .in_inline_code = m->in_inline_code,
    };
}

static void emit_text(struct md_renderer *m, const char *s, size_t n)
{
    /* Track the source bytes before the wrap layer substitutes tabs. */
    for (size_t j = 0; j < n; j++) {
        if (s[j] == ' ')
            m->trailing_spaces++;
        else if (s[j] != '\r')
            m->trailing_spaces = 0;
    }

    struct md_wrap_context ctx = wrap_context(m);
    int verbatim = m->in_code_fence || m->in_heading;
    md_wrap_emit_text(&m->wrap, &ctx, s, n, verbatim);
}

static void emit_raw(struct md_renderer *m, const char *s)
{
    if (!m->styled)
        return;
    struct md_wrap_context ctx = wrap_context(m);
    int verbatim = m->in_code_fence || m->in_heading;
    md_wrap_emit_raw(&m->wrap, &ctx, s, strlen(s), verbatim);
}

/* Delimiters bypass emit_text, so their helpers must clear trailing_spaces. */
static void open_bold(struct md_renderer *m)
{
    if (!m->suppress_bold)
        emit_raw(m, ANSI_BOLD);
    m->in_bold = 1;
    m->trailing_spaces = 0;
}

static void close_bold(struct md_renderer *m)
{
    if (!m->suppress_bold)
        emit_raw(m, ANSI_BOLD_OFF);
    m->in_bold = 0;
    m->trailing_spaces = 0;
    /* SGR 22 also closes the heading's bold; restore its full theme. */
    if (m->in_heading && !m->suppress_bold)
        emit_raw(m, theme_open(THEME_HEADING));
}

static void open_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC);
    m->in_italic = 1;
    m->trailing_spaces = 0;
}

static void close_italic(struct md_renderer *m)
{
    emit_raw(m, ANSI_ITALIC_OFF);
    m->in_italic = 0;
    m->trailing_spaces = 0;
}

static void open_inline_code(struct md_renderer *m)
{
    emit_raw(m, theme_open(THEME_CODE_INLINE));
    m->in_inline_code = 1;
    m->trailing_spaces = 0;
}

static void close_inline_code(struct md_renderer *m)
{
    emit_raw(m, theme_close(THEME_CODE_INLINE));
    m->in_inline_code = 0;
    m->trailing_spaces = 0;
    /* A code closer resets foreground, so restore a colored heading theme. */
    if (m->in_heading && strcmp(theme_open(THEME_HEADING), ANSI_BOLD) != 0)
        emit_raw(m, theme_open(THEME_HEADING));
}

static void open_code_fence(struct md_renderer *m)
{
    /* Set first so the opening escape bypasses the wrapper's reflow buffer. */
    m->in_code_fence = 1;
    emit_raw(m, theme_open(THEME_CODE_BLOCK));
}

static void close_code_fence(struct md_renderer *m)
{
    emit_raw(m, theme_close(THEME_CODE_BLOCK));
    m->in_code_fence = 0;
}

static void open_heading(struct md_renderer *m)
{
    /* Set first so the heading escape bypasses the wrapper's reflow buffer. */
    m->in_heading = 1;
    emit_raw(m, theme_open(THEME_HEADING));
}

static void close_heading(struct md_renderer *m)
{
    emit_raw(m, theme_close(THEME_HEADING));
    m->in_heading = 0;
}

/* ---------- table integration ---------- */

static void table_emit_direct(const char *bytes, size_t n, int is_raw, void *user)
{
    struct md_renderer *m = user;
    m->emit_cb(bytes, n, is_raw, m->user);
}

static void table_emit_text(void *user, const char *s, size_t n)
{
    emit_text(user, s, n);
}

static void table_emit_raw(void *user, const char *s, size_t n)
{
    struct md_renderer *m = user;
    if (!m->styled || n == 0)
        return;
    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_emit_raw(&m->wrap, &ctx, s, n, 0);
}

/* Cell escapes must update semantic state before the wrapper snapshots a break. */
static void table_replay_raw(void *user, const char *s, size_t n)
{
    struct md_renderer *m = user;
    size_t p = 0;
    while (p < n) {
        size_t q = p;
        while (q < n && s[q] != 'm')
            q++;
        if (q < n)
            q++;
        size_t len = q - p;
        const char *code_on = theme_open(THEME_CODE_INLINE);
        const char *code_off = theme_close(THEME_CODE_INLINE);
        if (len == strlen(ANSI_BOLD) && !memcmp(s + p, ANSI_BOLD, len))
            m->in_bold = 1;
        else if (len == strlen(ANSI_BOLD_OFF) && !memcmp(s + p, ANSI_BOLD_OFF, len))
            m->in_bold = 0;
        else if (len == strlen(ANSI_ITALIC) && !memcmp(s + p, ANSI_ITALIC, len))
            m->in_italic = 1;
        else if (len == strlen(ANSI_ITALIC_OFF) && !memcmp(s + p, ANSI_ITALIC_OFF, len))
            m->in_italic = 0;
        else if (len == strlen(code_on) && !memcmp(s + p, code_on, len))
            m->in_inline_code = 1;
        else if (len == strlen(code_off) && !memcmp(s + p, code_off, len))
            m->in_inline_code = 0;
        p = q;
    }
    table_emit_raw(m, s, n);
}

static void table_open_bold(void *user)
{
    open_bold(user);
}

static void table_close_bold(void *user)
{
    close_bold(user);
}

static void table_render_inline(void *user, const char *s, size_t n, int bold_base,
                                md_table_emit_fn emit, void *emit_user)
{
    struct md_renderer *m = user;
    struct md_renderer *sub = md_new(emit, emit_user, 0);
    if (!m->styled)
        md_set_styled(sub, 0);
    sub->inline_only = 1;
    sub->suppress_bold = bold_base;
    md_feed(sub, s, n);
    md_flush(sub);
    md_free(sub);
}

static void table_commit_pending(void *user)
{
    struct md_renderer *m = user;
    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_commit_pending(&m->wrap, &ctx);
}

static void table_row_reset(void *user)
{
    struct md_renderer *m = user;
    md_wrap_row_reset(&m->wrap);
}

static struct md_table_context table_context(struct md_renderer *m)
{
    return (struct md_table_context){
        .user = m,
        .emit_direct = table_emit_direct,
        .emit_text = table_emit_text,
        .emit_raw = table_emit_raw,
        .replay_raw = table_replay_raw,
        .open_bold = table_open_bold,
        .close_bold = table_close_bold,
        .render_inline = table_render_inline,
        .commit_pending = table_commit_pending,
        .row_reset = table_row_reset,
        .styled = m->styled,
        .wrap_width = md_wrap_width(&m->wrap),
    };
}

/* A handler consumes input, defers for lookahead, or passes to the next state. */
enum step_result {
    STEP_ADVANCED,
    STEP_DEFER,
    STEP_PASS,
};

/* ---------- thematic breaks ---------- */

/* Spaced dots distinguish model-authored dividers from solid system rules. */
#define HRULE_DOT_GAP 3              /* spaces between divider dots */
#define GLYPH_DOT     "\xc2\xb7"     /* · middle dot — model divider */
#define GLYPH_BULLET  "\xe2\x80\xa2" /* • list marker */

static void temit(struct md_renderer *m, const char *s, size_t n)
{
    if (n)
        m->emit_cb(s, n, 0, m->user);
}

static void temit_raw(struct md_renderer *m, const char *s)
{
    if (m->styled)
        m->emit_cb(s, strlen(s), 1, m->user);
}

static void temit_spaces(struct md_renderer *m, int n)
{
    static const char SP[] = "                                ";
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        temit(m, SP, (size_t)k);
        n -= k;
    }
}

/* Drop dots as needed to keep the divider on one line. */
static void render_hrule(struct md_renderer *m)
{
    int dots = 3;
    while (md_wrap_width(&m->wrap) > 0 && dots > 1 &&
           dots + (dots - 1) * HRULE_DOT_GAP > md_wrap_width(&m->wrap))
        dots--;
    temit_raw(m, ANSI_DIM);
    for (int k = 0; k < dots; k++) {
        if (k)
            temit_spaces(m, HRULE_DOT_GAP);
        temit(m, GLYPH_DOT, 2);
    }
    temit_raw(m, ANSI_BOLD_OFF); /* SGR 22 closes dim */
    temit(m, "\n", 1);
}

/* Emit a dim list bullet through the wrapper so hanging indentation sees it. */
static void emit_bullet(struct md_renderer *m)
{
    emit_raw(m, ANSI_DIM);
    emit_text(m, GLYPH_BULLET " ", 4);
    emit_raw(m, ANSI_BOLD_OFF); /* SGR 22 closes dim */
}

/* ---------- per-state byte dispatch ---------- */

/* Fence closers allow three leading spaces and require enough backticks followed only by
 * whitespace. Consume the marker line outside the styled content. */
static enum step_result step_in_code_fence(struct md_renderer *m, struct buf *w, size_t *i,
                                           int final)
{
    char c = w->data[*i];

    if (m->at_line_start) {
        size_t scan = *i;
        size_t sp = 0;
        while (scan < w->len && sp < 3 && w->data[scan] == ' ') {
            scan++;
            sp++;
        }
        if (scan >= w->len) {
            if (!final)
                return STEP_DEFER;
        } else if (w->data[scan] == '`') {
            size_t cnt = 0;
            while (scan + cnt < w->len && w->data[scan + cnt] == '`')
                cnt++;
            if (scan + cnt >= w->len && !final)
                return STEP_DEFER;
            if (cnt >= m->fence_open_count) {
                /* Include \r so CRLF-terminated closers are recognized. */
                size_t s2 = scan + cnt;
                while (s2 < w->len && w->data[s2] != '\n' &&
                       (w->data[s2] == ' ' || w->data[s2] == '\t' || w->data[s2] == '\r'))
                    s2++;
                if (s2 >= w->len) {
                    if (!final)
                        return STEP_DEFER;
                    /* EOF terminates a valid closer followed only by optional whitespace. */
                    close_code_fence(m);
                    m->fence_open_count = 0;
                    *i = s2;
                    m->at_line_start = 1;
                    return STEP_ADVANCED;
                }
                if (w->data[s2] == '\n') {
                    close_code_fence(m);
                    m->fence_open_count = 0;
                    *i = s2 + 1;
                    m->at_line_start = 1;
                    return STEP_ADVANCED;
                }
                /* A closer cannot have an info string. */
            }
        }
    }
    emit_text(m, &c, 1);
    m->at_line_start = (c == '\n');
    (*i)++;
    return STEP_ADVANCED;
}

/* A newline closes inline code, then passes through normal newline handling. */
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

static enum step_result step_line_start(struct md_renderer *m, struct buf *w, size_t *i, int final)
{
    struct md_line_info line = md_scan_line(w->data + *i, w->len - *i, final);
    int normalize_indent = line.normalize_indent;
    /* A bare setext underline stays literal at EOF, including its source indent. */
    if (final && line.kind == MD_LINE_THEMATIC && line.marker == '=')
        normalize_indent = 0;
    if (normalize_indent)
        *i += line.indent_length;
    /* After requested normalization, indented incomplete prefixes defer here; unindented
     * dispatchers can resolve their own lookahead. */
    if (!line.classification_complete && line.indent_length > 0)
        return STEP_DEFER;

    if (final && line.kind == MD_LINE_TEXT) {
        /* A block prefix that remained ambiguous through EOF falls back to literal text. */
        struct md_line_info streaming = md_scan_line(w->data + *i, w->len - *i, 0);
        if (!streaming.classification_complete) {
            emit_text(m, w->data + *i, w->len - *i);
            *i = w->len;
            return STEP_ADVANCED;
        }
    }

    if (final && line.kind == MD_LINE_THEMATIC) {
        if (line.marker == '=')
            emit_text(m, w->data + *i, w->len - *i);
        else
            render_hrule(m);
        *i = w->len;
        m->at_line_start = 1;
        return STEP_ADVANCED;
    }

    char c = w->data[*i];

    /* Stay at line start so blank-line runs collapse instead of soft-joining. */
    if (c == '\n') {
        if (!m->at_blank) {
            emit_text(m, "\n", 1);
            m->at_blank = 1;
        }
        m->at_line_start = 1;
        m->cur_line_is_block = 0;
        (*i)++;
        return STEP_ADVANCED;
    }
    m->at_blank = 0;

    if (c == '`') {
        /* Wider fences may contain shorter backtick runs. */
        size_t cnt = 0;
        while (*i + cnt < w->len && w->data[*i + cnt] == '`')
            cnt++;
        if (*i + cnt >= w->len)
            return STEP_DEFER;
        if (cnt >= 3) {
            /* Defer until the complete info line is available. */
            size_t scan = *i + cnt;
            while (scan < w->len && w->data[scan] != '\n')
                scan++;
            if (scan >= w->len)
                return STEP_DEFER;
            open_code_fence(m);
            m->fence_open_count = cnt;
            *i = scan + 1;
            m->at_line_start = 1;
            return STEP_ADVANCED;
        }
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
    }

    /* Consume marker-only lines whole so their runs are not parsed as emphasis. `=` runs remain
     * literal; the other thematic markers render as dividers. */
    if (c == '-' || c == '*' || c == '_' || c == '=') {
        size_t k = *i;
        size_t count = 0;
        while (k < w->len &&
               (w->data[k] == c || w->data[k] == ' ' || w->data[k] == '\t' || w->data[k] == '\r')) {
            if (w->data[k] == c)
                count++;
            k++;
        }
        if (k >= w->len)
            return STEP_DEFER;
        if (w->data[k] == '\n' && count >= 3) {
            if (c != '=')
                render_hrule(m);
            else
                emit_text(m, w->data + *i, k - *i + 1);
            *i = k + 1;
            m->at_line_start = 1;
            return STEP_ADVANCED;
        }
    }

    /* Normalize unordered markers to a dim bullet; preserve indent for nesting and hanging
     * continuation lines. */
    {
        size_t k = *i;
        while (k < w->len && w->data[k] == ' ')
            k++;
        if (k < w->len && (w->data[k] == '*' || w->data[k] == '-' || w->data[k] == '+')) {
            if (k + 1 >= w->len)
                return STEP_DEFER;
            if (w->data[k + 1] == ' ') {
                /* Collapse marker padding across feeds instead of deferring output. */
                size_t sp = k + 1;
                while (sp < w->len && w->data[sp] == ' ')
                    sp++;
                if (k > *i)
                    emit_text(m, w->data + *i, k - *i);
                emit_bullet(m);
                m->at_line_start = 0;
                m->skip_pad = (sp >= w->len);
                *i = sp;
                return STEP_ADVANCED;
            }
        }
    }

    /* Dim ordered markers while preserving their number and indentation. */
    {
        size_t k = *i;
        while (k < w->len && w->data[k] == ' ')
            k++;
        if (k < w->len && w->data[k] >= '0' && w->data[k] <= '9') {
            size_t d = k;
            while (d < w->len && w->data[d] >= '0' && w->data[d] <= '9')
                d++;
            if (d >= w->len || d + 1 >= w->len)
                return STEP_DEFER;
            if ((w->data[d] == '.' || w->data[d] == ')') && w->data[d + 1] == ' ') {
                /* Collapse marker padding across feeds instead of deferring output. */
                size_t sp = d + 1;
                while (sp < w->len && w->data[sp] == ' ')
                    sp++;
                if (k > *i)
                    emit_text(m, w->data + *i, k - *i);
                emit_raw(m, ANSI_DIM);
                emit_text(m, w->data + k, (d - k) + 1);
                emit_text(m, " ", 1);
                emit_raw(m, ANSI_BOLD_OFF); /* SGR 22 closes dim */
                m->at_line_start = 0;
                m->skip_pad = (sp >= w->len);
                *i = sp;
                return STEP_ADVANCED;
            }
        }
    }

    /* Only leading-pipe tables are detected; probing every mid-line pipe for a delimiter row
     * would stall ordinary prose by one line. */
    if (c == '|') {
        enum md_table_result r = md_table_try_start(&m->table, w, i);
        if (r == MD_TABLE_DEFER) {
            if (!final)
                return STEP_DEFER;
            /* md_table_finish already had the final chance; unresolved bytes stay literal. */
            emit_text(m, w->data + *i, w->len - *i);
            *i = w->len;
            return STEP_ADVANCED;
        }
        if (r == MD_TABLE_ADVANCED)
            return STEP_ADVANCED;
    }

    /* Preserve these markers, but isolate their lines from surrounding prose. */
    if (c == '>' || c == '|') {
        m->cur_line_is_block = 1;
        return STEP_PASS;
    }

    return STEP_PASS;
}

/* Reject intraword and whitespace-flanked emphasis without full delimiter-run parsing. */
static int can_open_emphasis(char left, char right)
{
    return !is_alnum(left) && !is_space(right);
}

static enum step_result step_inline(struct md_renderer *m, struct buf *w, size_t *i, int final)
{
    char c = w->data[*i];
    size_t remaining = w->len - *i;

    /* Continue collapsing list-marker padding from the previous feed. */
    if (m->skip_pad) {
        if (c == ' ') {
            (*i)++;
            return STEP_ADVANCED;
        }
        m->skip_pad = 0;
    }

    if (c == '\n') {
        if (m->in_heading) {
            close_heading(m);
            emit_text(m, "\n", 1);
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            (*i)++;
            return STEP_ADVANCED;
        }
        /* The scanner can identify a block boundary before its exact kind is complete, keeping
         * boundary output eager while line dispatch waits for more. */
        struct md_line_info line = md_scan_line(w->data + *i + 1, remaining - 1, final);
        if (line.kind == MD_LINE_INCOMPLETE) {
            /* An all-whitespace prefix remains ambiguous at any indentation depth. */
            if (line.indent_length == remaining - 1 || line.indent_length <= 3)
                return STEP_DEFER;
        }
        /* EOF resolves whitespace, thematic lines, and ambiguous prefixes without another
         * deferred tail. */
        if (final && line.kind == MD_LINE_TEXT && line.indent_length == remaining - 1) {
            emit_text(m, "\n", 1);
            *i = w->len;
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            return STEP_ADVANCED;
        }
        if (final && line.kind == MD_LINE_THEMATIC) {
            emit_text(m, "\n", 1);
            *i += 1 + line.indent_length;
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            return STEP_ADVANCED;
        }
        if (final && line.kind == MD_LINE_TEXT) {
            if (m->trailing_spaces >= 2) {
                emit_text(m, "\n", 1);
                emit_text(m, w->data + *i + 1, remaining - 1);
            } else {
                char prev = *i > 0 ? w->data[*i - 1] : m->prev_byte;
                if (prev != ' ' && prev != '\t' && prev != 0)
                    emit_text(m, " ", 1);
                size_t scan = *i + 1 + line.indent_length;
                emit_text(m, w->data + scan, w->len - scan);
            }
            *i = w->len;
            return STEP_ADVANCED;
        }

        int hard = 0;
        if (line.kind == MD_LINE_BLANK || line.kind == MD_LINE_HEADING ||
            line.kind == MD_LINE_FENCE || line.kind == MD_LINE_THEMATIC ||
            line.kind == MD_LINE_BLOCKQUOTE || line.kind == MD_LINE_PIPE)
            hard = 1;
        if ((line.kind == MD_LINE_BULLET || line.kind == MD_LINE_ORDERED) &&
            line.indent_length <= 3)
            hard = 1;
        if (m->cur_line_is_block)
            hard = 1;

        size_t scan = *i + 1 + line.indent_length;
        int normalize_indent = line.normalize_indent;
        if (hard) {
            if (m->in_bold)
                close_bold(m);
            if (m->in_italic)
                close_italic(m);
            emit_text(m, "\n", 1);
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            *i = normalize_indent ? scan : *i + 1;
            return STEP_ADVANCED;
        }
        /* Block lookahead takes precedence over an inline hard break. The tracked space count
         * works across feeds while preserving eager output. */
        if (m->trailing_spaces >= 2) {
            emit_text(m, "\n", 1);
            m->at_line_start = 1;
            m->cur_line_is_block = 0;
            (*i)++;
            return STEP_ADVANCED;
        }
        /* Join wrapped prose with one space, preserving inline styles. */
        size_t k = *i + 1;
        while (k < w->len && (w->data[k] == ' ' || w->data[k] == '\t'))
            k++;
        if (k >= w->len)
            return STEP_DEFER;
        char prev = *i > 0 ? w->data[*i - 1] : m->prev_byte;
        if (prev != ' ' && prev != '\t')
            emit_text(m, " ", 1);
        *i = k;
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
            if (remaining < 3) {
                if (!final)
                    return STEP_DEFER;
                emit_text(m, "**", 2);
                *i += 2;
                return STEP_ADVANCED;
            }
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
        if (remaining < 2) {
            if (!final)
                return STEP_DEFER;
            /* A lone final star closes active italic but is otherwise literal. */
            if (m->in_italic)
                close_italic(m);
            else
                emit_text(m, &c, 1);
            (*i)++;
            return STEP_ADVANCED;
        }
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
        if (remaining < 2) {
            if (!final)
                return STEP_DEFER;
            emit_text(m, &c, 1);
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

    /* Coalesce plain text so the wrapper receives words rather than individual bytes. */
    size_t end = *i + 1;
    while (end < w->len) {
        char d = w->data[end];
        if (d == '\n' || d == '`' || d == '*' || d == '_')
            break;
        end++;
    }
    emit_text(m, w->data + *i, end - *i);
    *i = end;
    return STEP_ADVANCED;
}

struct md_renderer *md_new(md_emit_fn emit_cb, void *user, int wrap_width)
{
    struct md_renderer *m = xcalloc(1, sizeof(*m));
    m->emit_cb = emit_cb;
    m->user = user;
    m->at_line_start = 1;
    m->at_blank = 1;
    m->skip_pad = 0;
    m->styled = 1;
    md_wrap_reset(&m->wrap, wrap_width);
    md_table_reset(&m->table);
    return m;
}

void md_reset(struct md_renderer *m, int wrap_width)
{
    buf_reset(&m->tail);
    m->prev_byte = 0;
    m->at_line_start = 1;
    m->trailing_spaces = 0;
    m->fence_open_count = 0;
    m->in_heading = 0;
    m->in_code_fence = 0;
    m->in_inline_code = 0;
    m->in_bold = 0;
    m->in_italic = 0;
    m->styled = 1;
    m->cur_line_is_block = 0;
    m->at_blank = 1;
    m->skip_pad = 0;
    md_wrap_reset(&m->wrap, wrap_width);
    md_table_reset(&m->table);
    m->inline_only = 0;
    m->suppress_bold = 0;
}

void md_free(struct md_renderer *m)
{
    if (!m)
        return;
    buf_free(&m->tail);
    md_wrap_free(&m->wrap);
    md_table_free(&m->table);
    free(m);
}

/* Final mode resolves deferred prefixes against EOF instead of saving another tail. */
static void md_process(struct md_renderer *m, const char *s, size_t n, int final)
{
    /* Present deferred and new input as one contiguous stream. */
    struct buf w;
    buf_init(&w);
    if (m->tail.len) {
        buf_append(&w, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }
    if (n)
        buf_append(&w, s, n);

    size_t i = 0;
    while (i < w.len) {
        enum step_result step;

        if (md_table_is_collecting(&m->table)) {
            struct md_table_context ctx = table_context(m);
            enum md_table_result table_step = md_table_step(&m->table, &ctx, &w, &i);
            if (table_step == MD_TABLE_DEFER)
                break;
            if (table_step == MD_TABLE_ADVANCED)
                continue;
            m->at_line_start = 1;
        }

        if (m->in_code_fence) {
            step = step_in_code_fence(m, &w, &i, final);
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
            /* Normal newline handling resumes after inline code closes. */
        }

        if (m->at_line_start && !m->inline_only) {
            step = step_line_start(m, &w, &i, final);
            if (step == STEP_DEFER)
                break;
            if (step == STEP_ADVANCED)
                continue;
            m->at_line_start = 0;
        }

        /* step_inline must advance or defer; STEP_PASS would spin without changing i. */
        step = step_inline(m, &w, &i, final);
        if (step == STEP_DEFER)
            break;
    }

    /* Keep ambiguous input deferred; incomplete table rows must remain lossless and ordered. */
    size_t rem = w.len - i;
    struct md_table_context table_ctx = table_context(m);
    if (rem > 0 && md_table_bail_partial(&m->table, &table_ctx, w.data + i, rem)) {
        i = w.len;
        rem = 0;
    }
    if (final && rem > 0) {
        /* Preserve bytes literally if a handler unexpectedly still defers at EOF. */
        emit_text(m, w.data + i, rem);
        i = w.len;
        rem = 0;
    }
    if (rem > TAIL_MAX && !md_table_is_collecting(&m->table)) {
        emit_text(m, w.data + i, rem - TAIL_MAX);
        i += rem - TAIL_MAX;
        rem = TAIL_MAX;
    }
    if (rem > 0)
        buf_append(&m->tail, w.data + i, rem);

    /* Preserve the left neighbor for emphasis at the next feed boundary. */
    if (i > 0)
        m->prev_byte = w.data[i - 1];

    buf_free(&w);
}

void md_feed(struct md_renderer *m, const char *s, size_t n)
{
    md_process(m, s, n, 0);
}

void md_flush(struct md_renderer *m)
{
    int was_collecting_table = md_table_is_collecting(&m->table);
    struct md_table_context table_ctx = table_context(m);
    md_table_finish(&m->table, &table_ctx, &m->tail);
    /* A rejected final row is table fallback output, not fresh Markdown input. */
    if (was_collecting_table && m->tail.len > 0) {
        emit_text(m, m->tail.data, m->tail.len);
        buf_reset(&m->tail);
    }
    md_process(m, NULL, 0, 1);

    /* Never leave terminal styling open at EOF. */
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

    struct md_wrap_context ctx = wrap_context(m);
    md_wrap_flush(&m->wrap, &ctx);
}

void md_set_styled(struct md_renderer *m, int on)
{
    if (m->styled == on)
        return;
    /* Resolve deferred markers under the old SGR mode, then reset at the caller's output seam. */
    md_flush(m);
    md_reset(m, md_wrap_width(&m->wrap));
    m->styled = on;
}

int md_in_table(const struct md_renderer *m)
{
    /* The agent surfaces a spinner during otherwise-silent accumulation. */
    return m && md_table_is_collecting(&m->table);
}
