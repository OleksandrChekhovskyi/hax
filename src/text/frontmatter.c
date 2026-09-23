/* SPDX-License-Identifier: MIT */
#include "text/frontmatter.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "text/fmt.h"
#include "text/utf8.h"
#include "text/utf8_sanitize.h"

/* YAML's own separation whitespace, which delimits comments, keys, and continuation lines. */
static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *skip_to_line_end(const char *p, const char *end)
{
    const char *newline = memchr(p, '\n', end - p);
    return newline ? newline : end;
}

/* YAML's single-character escapes and the codepoints they stand for. */
static const struct {
    char indicator;
    uint32_t codepoint;
} NAMED_ESCAPES[] = {
    {'0', 0x00}, {'a', 0x07},  {'b', 0x08}, {'t', 0x09}, {'\t', 0x09},  {'n', 0x0A},
    {'v', 0x0B}, {'f', 0x0C},  {'r', 0x0D}, {'e', 0x1B}, {' ', 0x20},   {'"', 0x22},
    {'/', 0x2F}, {'\\', 0x5C}, {'N', 0x85}, {'_', 0xA0}, {'L', 0x2028}, {'P', 0x2029},
};

/* Store the codepoint of the escape whose indicator is at p and return the escape's length, or zero
 * when it is not a valid escape. */
static size_t parse_escape(const char *p, const char *end, uint32_t *codepoint)
{
    for (size_t i = 0; i < sizeof(NAMED_ESCAPES) / sizeof(NAMED_ESCAPES[0]); i++) {
        if (NAMED_ESCAPES[i].indicator == *p) {
            *codepoint = NAMED_ESCAPES[i].codepoint;
            return 1;
        }
    }
    size_t hex_digits = *p == 'x' ? 2 : *p == 'u' ? 4 : *p == 'U' ? 8 : 0;
    if (hex_digits == 0 || (size_t)(end - p - 1) < hex_digits ||
        !parse_hex(p + 1, hex_digits, codepoint))
        return 0;
    return 1 + hex_digits;
}

/* An escaped line break joins the lines without a space, dropping the next line's indentation. */
static const char *skip_escaped_line_break(const char *p, const char *end)
{
    if (*p == '\r' && p + 1 < end && p[1] == '\n')
        p++;
    p++;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

/* Decode the escape whose indicator is at p and return the position after it. Invalid escapes,
 * including codepoints UTF-8 cannot encode, stay verbatim. */
static const char *decode_escape(struct buf *out, const char *p, const char *end)
{
    if (*p == '\r' || *p == '\n')
        return skip_escaped_line_break(p, end);

    uint32_t codepoint;
    char encoded[4];
    size_t escape_len = parse_escape(p, end, &codepoint);
    size_t encoded_len = escape_len > 0 ? utf8_encode(codepoint, encoded) : 0;
    if (encoded_len == 0) {
        buf_append(out, "\\", 1);
        return p;
    }
    buf_append(out, encoded, encoded_len);
    return p + escape_len;
}

/* Each decode_* reader starts past its scalar's indicator and returns -1 for malformed input. */

static int decode_double_quoted(struct buf *out, const char *p, const char *end)
{
    while (p < end) {
        char c = *p++;
        if (c == '"')
            return 0;
        if (c == '\\' && p < end)
            p = decode_escape(out, p, end);
        else
            buf_append(out, &c, 1);
    }
    return -1;
}

static int decode_single_quoted(struct buf *out, const char *p, const char *end)
{
    for (; p < end; p++) {
        if (*p == '\'') {
            if (p + 1 == end || p[1] != '\'')
                return 0;
            p++;
        }
        buf_append(out, p, 1);
    }
    return -1;
}

/* Literal and folded styles differ only in line breaks, and chomping only in trailing ones, so
 * folding onto one line makes the header's style and indicators irrelevant. */
static int decode_block(struct buf *out, const char *p, const char *end)
{
    while (p < end && (*p == '+' || *p == '-' || (*p >= '0' && *p <= '9')))
        p++;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p < end && *p == '#')
        p = skip_to_line_end(p, end);
    if (p < end && *p != '\r' && *p != '\n')
        return -1;
    buf_append(out, p, end - p);
    return 0;
}

static void decode_plain(struct buf *out, const char *start, const char *end)
{
    const char *p = start;
    /* A comment ends a plain scalar, but `#` inside a word is text. */
    while (p < end && !(*p == '#' && p > start && is_space(p[-1])))
        p++;
    buf_append(out, start, p - start);
}

/* The value starts right after the key's colon, so a leading `#` always follows whitespace and
 * starts a comment. */
static int decode_scalar(struct buf *out, const char *p, const char *end)
{
    for (;;) {
        while (p < end && is_space(*p))
            p++;
        if (p == end || *p != '#')
            break;
        p = skip_to_line_end(p, end);
    }
    if (p == end)
        return 0;

    switch (*p) {
    case '"':
        return decode_double_quoted(out, p + 1, end);
    case '\'':
        return decode_single_quoted(out, p + 1, end);
    case '|':
    case '>':
        return decode_block(out, p + 1, end);
    default:
        decode_plain(out, p, end);
        return 0;
    }
}

/* Return the byte length of the blank or line break at p, or zero. Beyond ASCII controls, this
 * covers NEL, LS, and PS, which YAML 1.2 keeps as content but which would still break the line. */
static size_t blank_len(const char *p, const char *end)
{
    static const char *const unicode_breaks[] = {"\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9"};
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '\f' || *p == '\r')
        return 1;
    for (size_t i = 0; i < sizeof(unicode_breaks) / sizeof(unicode_breaks[0]); i++) {
        size_t len = strlen(unicode_breaks[i]);
        if ((size_t)(end - p) >= len && memcmp(p, unicode_breaks[i], len) == 0)
            return len;
    }
    return 0;
}

/* Collapse each run of blanks and line breaks to one space and trim both ends. Returns NULL when
 * nothing remains. */
static char *fold_line(const char *text)
{
    const char *end = text + strlen(text);
    struct buf line;
    buf_init(&line);
    int space_pending = 0;
    for (const char *p = text; p < end;) {
        size_t space_len = blank_len(p, end);
        if (space_len > 0) {
            space_pending = line.len > 0;
            p += space_len;
            continue;
        }
        if (space_pending) {
            buf_append(&line, " ", 1);
            space_pending = 0;
        }
        buf_append(&line, p, 1);
        p++;
    }
    if (line.len == 0) {
        buf_free(&line);
        return NULL;
    }
    return buf_steal(&line);
}

static char *read_value(const char *value, const char *end)
{
    struct buf decoded;
    buf_init(&decoded);
    char *line = NULL;
    if (decode_scalar(&decoded, value, end) == 0 && decoded.len > 0) {
        /* Sanitize first so that folding matches line breaks only as whole codepoints. */
        char *text = utf8_sanitize(decoded.data, decoded.len);
        line = fold_line(text);
        free(text);
    }
    buf_free(&decoded);
    return line;
}

static int is_fence(const char *line, size_t line_len)
{
    return (line_len == 3 && memcmp(line, "---", 3) == 0) ||
           (line_len == 4 && memcmp(line, "---\r", 4) == 0);
}

static int is_key_line(const char *line, size_t line_len, const char *key, size_t key_len)
{
    return line_len > key_len && memcmp(line, key, key_len) == 0 && line[key_len] == ':' &&
           (line_len == key_len + 1 || is_space(line[key_len + 1]));
}

char *frontmatter_scalar_line(const char *content, size_t content_len, const char *key)
{
    const char *end = content + content_len;
    const char *cursor;
    if (content_len >= 4 && memcmp(content, "---\n", 4) == 0)
        cursor = content + 4;
    else if (content_len >= 5 && memcmp(content, "---\r\n", 5) == 0)
        cursor = content + 5;
    else
        return NULL;

    /* A value spans its key's line and every following indented or blank line. */
    size_t key_len = strlen(key);
    const char *value = NULL;
    const char *value_end = NULL;
    int in_value = 0;
    while (cursor < end) {
        const char *line_end = skip_to_line_end(cursor, end);
        size_t line_len = line_end - cursor;
        if (is_fence(cursor, line_len))
            return value ? read_value(value, value_end) : NULL;

        if (in_value && (line_len == 0 || is_space(*cursor))) {
            value_end = line_end;
        } else if (!value && is_key_line(cursor, line_len, key, key_len)) {
            value = cursor + key_len + 1;
            value_end = line_end;
            in_value = 1;
        } else {
            in_value = 0;
        }

        if (line_end == end)
            break;
        cursor = line_end + 1;
    }
    return NULL;
}
