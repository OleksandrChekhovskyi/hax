/* SPDX-License-Identifier: MIT */
#include "text/html_markdown.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

struct hstate {
    struct buf out;
    int in_pre;         /* depth of open <pre> — text is kept verbatim inside */
    int anchor_depth;   /* open <a href> that emitted '[' and owes a "](href)" */
    size_t anchor_mark; /* out.len just after the '[' — to detect empty link text */
    char *anchor_href;
};

/* Encode a Unicode code point as UTF-8 into `b`. Invalid/oversized values
 * are dropped silently — they only arise from malformed numeric entities. */
static void append_cp(struct buf *b, unsigned cp)
{
    unsigned char t[4];
    if (cp < 0x80) {
        t[0] = (unsigned char)cp;
        buf_append(b, t, 1);
    } else if (cp < 0x800) {
        t[0] = (unsigned char)(0xC0 | (cp >> 6));
        t[1] = (unsigned char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (unsigned char)(0xE0 | (cp >> 12));
        t[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (unsigned char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 3);
    } else if (cp <= 0x10FFFF) {
        t[0] = (unsigned char)(0xF0 | (cp >> 18));
        t[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (unsigned char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 4);
    }
}

/* A handful of named entities cover the overwhelming majority of real-world
 * prose; anything else is left as its literal source text. nbsp maps to a
 * plain space so it collapses with surrounding whitespace. */
static const struct {
    const char *name;
    unsigned cp;
} ENTITIES[] = {
    {"amp", '&'},       {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", ' '},      {"copy", 0xA9},    {"reg", 0xAE},     {"mdash", 0x2014}, {"ndash", 0x2013},
    {"hellip", 0x2026}, {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
};

/* Decode one `&...;` entity at `p` (p[0]=='&'). Appends the decoded bytes to
 * `b` and returns the number of input bytes consumed. On anything that isn't
 * a recognizable entity, appends a literal '&' and returns 1. */
static size_t decode_entity(struct buf *b, const char *p, size_t max)
{
    /* Find the terminating ';' within a short window — a stray '&' in prose
     * shouldn't swallow the rest of the line. */
    size_t semi = 0;
    size_t limit = max < 32 ? max : 32;
    for (size_t i = 1; i < limit; i++) {
        if (p[i] == ';') {
            semi = i;
            break;
        }
        if (p[i] == '&' || isspace((unsigned char)p[i]))
            break;
    }
    if (!semi) {
        buf_append(b, "&", 1);
        return 1;
    }

    if (p[1] == '#') {
        unsigned cp;
        char *end;
        if (p[2] == 'x' || p[2] == 'X')
            cp = (unsigned)strtoul(p + 3, &end, 16);
        else
            cp = (unsigned)strtoul(p + 2, &end, 10);
        if (cp == 0 || end == p + 2) {
            buf_append(b, "&", 1);
            return 1;
        }
        append_cp(b, cp);
        return semi + 1;
    }

    size_t name_len = semi - 1;
    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
        if (strlen(ENTITIES[i].name) == name_len &&
            strncmp(ENTITIES[i].name, p + 1, name_len) == 0) {
            append_cp(b, ENTITIES[i].cp);
            return semi + 1;
        }
    }
    buf_append(b, "&", 1);
    return 1;
}

/* Append a text run, decoding entities. Outside <pre>, runs of whitespace
 * collapse to a single space and a leading space is dropped when the output
 * already ends in space/newline (or is empty). Inside <pre>, bytes are kept
 * verbatim. */
static void append_text(struct hstate *st, const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (c == '&') {
            size_t before = st->out.len;
            i += decode_entity(&st->out, s + i, n - i);
            /* A whitespace-producing entity (nbsp) must collapse like literal
             * whitespace when outside <pre>. */
            if (!st->in_pre && st->out.len == before + 1 && st->out.data[before] == ' ' &&
                (before == 0 || st->out.data[before - 1] == ' ' ||
                 st->out.data[before - 1] == '\n'))
                st->out.len = before;
            continue;
        }
        if (!st->in_pre && isspace((unsigned char)c)) {
            char last = st->out.len ? st->out.data[st->out.len - 1] : '\n';
            if (last != ' ' && last != '\n')
                buf_append(&st->out, " ", 1);
            i++;
            continue;
        }
        buf_append(&st->out, &c, 1);
        i++;
    }
}

/* Ensure the output ends with at least `n` trailing newlines (n is 1 for a
 * line break, 2 for a paragraph/block boundary). No-op on an empty buffer so
 * the document never starts with blank lines. */
static void ensure_newlines(struct hstate *st, int n)
{
    if (!st->out.len)
        return;
    int have = 0;
    for (size_t i = st->out.len; i > 0 && st->out.data[i - 1] == '\n'; i--)
        have++;
    for (; have < n; have++)
        buf_append(&st->out, "\n", 1);
}

/* Case-insensitive match of `name`/`len` against a NUL-terminated lowercase
 * literal. */
static int tag_is(const char *name, size_t len, const char *lit)
{
    return strlen(lit) == len && strncasecmp(name, lit, len) == 0;
}

/* These elements carry no prose; their entire content is discarded. `nav` and
 * `aside` are boilerplate (navigation, sidebars) even when nested inside the
 * main content region — Trafilatura prunes them the same way — so they belong
 * here rather than in the block-break set. */
static int is_skip_tag(const char *name, size_t len)
{
    static const char *const skip[] = {"script",   "style",  "head",   "noscript", "svg",
                                       "template", "iframe", "object", "embed",    "select",
                                       "textarea", "nav",    "aside"};
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (tag_is(name, len, skip[i]))
            return 1;
    return 0;
}

/* Find the index just past the next '>' starting at `from`, treating quoted
 * attribute values as opaque so a '>' inside an attribute doesn't end the
 * tag early. Returns `len` if unterminated. */
static size_t tag_end(const char *html, size_t len, size_t from)
{
    char quote = 0;
    for (size_t i = from; i < len; i++) {
        char c = html[i];
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return i + 1;
        }
    }
    return len;
}

/* Extract the href value from a tag's attribute region [from, end). Returns a
 * malloc'd, entity-decoded string or NULL when absent. */
static char *extract_href(const char *html, size_t from, size_t end)
{
    for (size_t i = from; i + 4 < end; i++) {
        if (strncasecmp(html + i, "href", 4) != 0)
            continue;
        size_t j = i + 4;
        while (j < end && isspace((unsigned char)html[j]))
            j++;
        if (j >= end || html[j] != '=')
            continue;
        j++;
        while (j < end && isspace((unsigned char)html[j]))
            j++;
        char quote = 0;
        if (j < end && (html[j] == '"' || html[j] == '\'')) {
            quote = html[j];
            j++;
        }
        size_t start = j;
        while (j < end) {
            char c = html[j];
            if (quote ? c == quote : (isspace((unsigned char)c) || c == '>'))
                break;
            j++;
        }
        /* Decode entities (mainly &amp; → &) but keep the value verbatim. */
        struct buf b;
        buf_init(&b);
        for (size_t k = start; k < j;) {
            if (html[k] == '&') {
                k += decode_entity(&b, html + k, j - k);
            } else {
                buf_append(&b, &html[k], 1);
                k++;
            }
        }
        return buf_steal(&b);
    }
    return NULL;
}

/* Find the matching "</name>" for a skip element starting after its open
 * tag, returning the index just past the close tag (or `len` if unterminated
 * — the rest of the document is then dropped, which is the safe choice for an
 * unbalanced script/style). */
static size_t skip_to_close(const char *html, size_t len, size_t from, const char *name,
                            size_t name_len)
{
    for (size_t i = from; i + 1 < len; i++) {
        if (html[i] != '<' || html[i + 1] != '/')
            continue;
        size_t j = i + 2;
        if (j + name_len <= len && strncasecmp(html + j, name, name_len) == 0)
            return tag_end(html, len, i + 2);
    }
    return len;
}

/* Collapse 3+ consecutive newlines to 2, strip trailing spaces before each
 * newline, and trim leading/trailing whitespace from the whole document. */
static char *tidy(const char *s, size_t n, size_t *out_len)
{
    /* Skip leading whitespace. */
    size_t start = 0;
    while (start < n && isspace((unsigned char)s[start]))
        start++;
    size_t endp = n;
    while (endp > start && isspace((unsigned char)s[endp - 1]))
        endp--;

    struct buf b;
    buf_init(&b);
    int nl = 0;
    size_t pending_spaces = 0;
    for (size_t i = start; i < endp; i++) {
        char c = s[i];
        if (c == '\n') {
            pending_spaces = 0; /* drop trailing spaces before newline */
            nl++;
            if (nl <= 2)
                buf_append(&b, "\n", 1);
            continue;
        }
        nl = 0;
        if (c == ' ') {
            pending_spaces++;
            continue;
        }
        for (; pending_spaces; pending_spaces--)
            buf_append(&b, " ", 1);
        buf_append(&b, &c, 1);
    }
    if (out_len)
        *out_len = b.len;
    char *r = buf_steal(&b);
    return r ? r : xstrdup(""); /* never NULL, even for empty output */
}

/* Below this many converted characters, a main-content region is treated as
 * a mis-pick and we fall back to the whole document (graceful degradation, as
 * in Trafilatura). */
#define MAIN_MIN_CHARS 200

/* Does an opening tag named `name` begin at html[pos]? Case-insensitive, with
 * a name boundary so `<main>` matches but `<mainbar>` doesn't. */
static int opens_named(const char *html, size_t len, size_t pos, const char *name)
{
    size_t nl = strlen(name);
    if (html[pos] != '<' || pos + 1 + nl > len || strncasecmp(html + pos + 1, name, nl) != 0)
        return 0;
    char c = (pos + 1 + nl < len) ? html[pos + 1 + nl] : '>';
    return c == '>' || c == '/' || isspace((unsigned char)c);
}

static int closes_named(const char *html, size_t len, size_t pos, const char *name)
{
    size_t nl = strlen(name);
    if (pos + 2 + nl > len || html[pos] != '<' || html[pos + 1] != '/' ||
        strncasecmp(html + pos + 2, name, nl) != 0)
        return 0;
    char c = (pos + 2 + nl < len) ? html[pos + 2 + nl] : '>';
    return c == '>' || isspace((unsigned char)c);
}

/* Given an opening `<name ...>` at `open`, set [*start,*stop) to its inner
 * content (after the open tag, before the matching close), counting nested
 * same-name elements so a `<div>`-in-`<div>` doesn't close early. */
static void region_of(const char *html, size_t len, size_t open, const char *name, size_t *start,
                      size_t *stop)
{
    size_t content = tag_end(html, len, open + 1);
    *start = content;
    int depth = 1;
    for (size_t i = content; i < len;) {
        if (html[i] != '<') {
            i++;
        } else if (closes_named(html, len, i, name)) {
            if (--depth == 0) {
                *stop = i;
                return;
            }
            i = tag_end(html, len, i + 1);
        } else if (opens_named(html, len, i, name)) {
            depth++;
            i = tag_end(html, len, i + 1);
        } else {
            i++;
        }
    }
    *stop = len; /* unbalanced — take the rest of the document */
}

/* Locate the page's main-content container so navigation/sidebar chrome that
 * sits outside it is never converted. Preference order matches what real doc
 * stacks emit: <main> (MDN, Wikipedia, modern themes), then an element with
 * role="main" (classic Sphinx's <div role="main">), then <article>. Returns 1
 * and sets [*start,*stop) on a hit; 0 (convert the whole document) otherwise. */
static int find_main_region(const char *html, size_t len, size_t *start, size_t *stop)
{
    for (size_t i = 0; i < len; i++)
        if (opens_named(html, len, i, "main")) {
            region_of(html, len, i, "main", start, stop);
            return 1;
        }

    for (size_t i = 0; i + 4 < len; i++) {
        if (strncasecmp(html + i, "role", 4) != 0)
            continue;
        if (i > 0 && (isalnum((unsigned char)html[i - 1]) || html[i - 1] == '-'))
            continue; /* part of a longer attribute like data-role */
        size_t j = i + 4;
        while (j < len && isspace((unsigned char)html[j]))
            j++;
        if (j >= len || html[j] != '=')
            continue;
        j++;
        while (j < len && isspace((unsigned char)html[j]))
            j++;
        if (j < len && (html[j] == '"' || html[j] == '\''))
            j++;
        if (j + 4 > len || strncasecmp(html + j, "main", 4) != 0)
            continue;
        char after = (j + 4 < len) ? html[j + 4] : '"';
        if (!(after == '"' || after == '\'' || after == '>' || isspace((unsigned char)after)))
            continue;
        size_t lt = i;
        while (lt > 0 && html[lt] != '<')
            lt--;
        if (html[lt] != '<' || lt + 1 >= len || html[lt + 1] == '/')
            continue;
        size_t ns = lt + 1, ne = ns;
        while (ne < len && (isalnum((unsigned char)html[ne]) || html[ne] == '-'))
            ne++;
        size_t nl = ne - ns;
        if (nl == 0 || nl >= 16)
            continue;
        char nm[16];
        memcpy(nm, html + ns, nl);
        nm[nl] = '\0';
        region_of(html, len, lt, nm, start, stop);
        return 1;
    }

    for (size_t i = 0; i < len; i++)
        if (opens_named(html, len, i, "article")) {
            region_of(html, len, i, "article", start, stop);
            return 1;
        }

    return 0;
}

/* Convert the byte range [start,stop) of `html` to Markdown. */
static char *convert_range(const char *html, size_t start, size_t stop, size_t *out_len)
{
    struct hstate st;
    buf_init(&st.out);
    st.in_pre = 0;
    st.anchor_depth = 0;
    st.anchor_mark = 0;
    st.anchor_href = NULL;

    size_t i = start;
    while (i < stop) {
        if (html[i] != '<') {
            size_t run = i;
            while (i < stop && html[i] != '<')
                i++;
            append_text(&st, html + run, i - run);
            continue;
        }

        /* Comment / doctype / processing instruction. */
        if (i + 3 < stop && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
            size_t k = i + 4;
            while (k + 2 < stop && !(html[k] == '-' && html[k + 1] == '-' && html[k + 2] == '>'))
                k++;
            i = (k + 2 < stop) ? k + 3 : stop;
            continue;
        }
        if (i + 1 < stop && (html[i + 1] == '!' || html[i + 1] == '?')) {
            i = tag_end(html, stop, i + 1);
            continue;
        }

        size_t j = i + 1;
        int closing = (j < stop && html[j] == '/');
        if (closing)
            j++;
        size_t name_start = j;
        while (j < stop && (isalnum((unsigned char)html[j]) || html[j] == '-'))
            j++;
        size_t name_len = j - name_start;
        const char *name = html + name_start;
        if (name_len == 0) {
            /* A bare '<' that isn't a tag — emit it literally. */
            buf_append(&st.out, "<", 1);
            i++;
            continue;
        }

        size_t end = tag_end(html, stop, j);

        if (!closing && is_skip_tag(name, name_len)) {
            i = skip_to_close(html, stop, end, name, name_len);
            continue;
        }

        /* Block / inline transforms. */
        if (tag_is(name, name_len, "br")) {
            ensure_newlines(&st, 1);
        } else if (tag_is(name, name_len, "hr")) {
            ensure_newlines(&st, 2);
            buf_append(&st.out, "---", 3);
            ensure_newlines(&st, 2);
        } else if (name_len == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] >= '1' &&
                   name[1] <= '6') {
            ensure_newlines(&st, 2);
            if (!closing) {
                int level = name[1] - '0';
                for (int k = 0; k < level; k++)
                    buf_append(&st.out, "#", 1);
                buf_append(&st.out, " ", 1);
            }
        } else if (tag_is(name, name_len, "li")) {
            if (!closing) {
                ensure_newlines(&st, 1);
                buf_append(&st.out, "- ", 2);
            }
        } else if (tag_is(name, name_len, "dt") || tag_is(name, name_len, "dd") ||
                   tag_is(name, name_len, "tr")) {
            /* Definition terms/descriptions and table rows each start a new
             * line — without this the cells of a status-code table or API
             * reference run together into one unreadable paragraph. */
            if (!closing)
                ensure_newlines(&st, 1);
        } else if (tag_is(name, name_len, "td") || tag_is(name, name_len, "th")) {
            /* Separate cells within a row with " | ". The first cell sits at
             * the start of the line (the <tr> already broke it), so it gets no
             * leading separator. Trim a trailing space first so we don't emit
             * "a  | b". This isn't a real Markdown table, just enough
             * structure to keep columns distinguishable. */
            if (!closing) {
                while (st.out.len && st.out.data[st.out.len - 1] == ' ')
                    st.out.len--;
                if (st.out.len && st.out.data[st.out.len - 1] != '\n')
                    buf_append(&st.out, " | ", 3);
            }
        } else if (tag_is(name, name_len, "pre")) {
            if (closing) {
                if (st.in_pre)
                    st.in_pre--;
                ensure_newlines(&st, 1);
                buf_append(&st.out, "```", 3);
                ensure_newlines(&st, 2);
            } else {
                ensure_newlines(&st, 2);
                buf_append(&st.out, "```", 3);
                buf_append(&st.out, "\n", 1);
                st.in_pre++;
            }
        } else if (tag_is(name, name_len, "code") && !st.in_pre) {
            buf_append(&st.out, "`", 1);
        } else if (tag_is(name, name_len, "a")) {
            if (closing) {
                if (st.anchor_depth) {
                    st.anchor_depth--;
                    if (st.out.len <= st.anchor_mark) {
                        /* No visible text between the brackets (a bare jump
                         * anchor like <a name="x">) — drop the stray '[' and
                         * skip the link entirely rather than emit "[](url)". */
                        st.out.len = st.anchor_mark - 1;
                    } else {
                        buf_append(&st.out, "](", 2);
                        if (st.anchor_href)
                            buf_append(&st.out, st.anchor_href, strlen(st.anchor_href));
                        buf_append(&st.out, ")", 1);
                    }
                    free(st.anchor_href);
                    st.anchor_href = NULL;
                }
            } else {
                char *href = extract_href(html, j, end > 0 ? end - 1 : j);
                if (href && *href && strncasecmp(href, "javascript:", 11) != 0) {
                    free(st.anchor_href);
                    st.anchor_href = href;
                    st.anchor_depth++;
                    buf_append(&st.out, "[", 1);
                    st.anchor_mark = st.out.len;
                } else {
                    free(href);
                }
            }
        } else if (tag_is(name, name_len, "p") || tag_is(name, name_len, "div") ||
                   tag_is(name, name_len, "section") || tag_is(name, name_len, "article") ||
                   tag_is(name, name_len, "header") || tag_is(name, name_len, "footer") ||
                   tag_is(name, name_len, "ul") || tag_is(name, name_len, "ol") ||
                   tag_is(name, name_len, "dl") || tag_is(name, name_len, "table") ||
                   tag_is(name, name_len, "blockquote") || tag_is(name, name_len, "main")) {
            ensure_newlines(&st, 2);
        }
        /* All other tags: stripped, no whitespace effect. */

        i = end;
    }

    if (st.anchor_depth) {
        /* Unbalanced <a>: drop the dangling '[' if it has no text, else close
         * it so we don't leave a stray opening bracket. */
        if (st.out.len <= st.anchor_mark)
            st.out.len = st.anchor_mark - 1;
        else
            buf_append(&st.out, "]", 1);
        free(st.anchor_href);
    }

    char *cleaned = tidy(st.out.data ? st.out.data : "", st.out.len, out_len);
    buf_free(&st.out);
    return cleaned;
}

char *html_to_markdown(const char *html, size_t len, size_t *out_len)
{
    size_t start = 0, stop = len;
    int scoped = find_main_region(html, len, &start, &stop);

    size_t n = 0;
    char *out = convert_range(html, start, stop, &n);

    /* If scoping to the main container produced almost nothing — a mis-marked
     * page, or content that lives outside it — convert the whole document
     * instead. Better a noisy full page than an empty one. */
    if (scoped && n < MAIN_MIN_CHARS) {
        free(out);
        out = convert_range(html, 0, len, &n);
    }
    if (out_len)
        *out_len = n;
    return out;
}
