/* SPDX-License-Identifier: MIT */
#include "model_sort.h"

#include <ctype.h>
#include <string.h>

/* Ranked by the desired order at the first divergence between two ids: a version
 * continuation outranks the bare id ("gpt-5.6" before "gpt-5"), which precedes its dated
 * snapshots, which precede named variants ("gpt-5-mini"). */
enum token_kind {
    TOKEN_VERSION,
    TOKEN_ID_END,
    TOKEN_DATE,
    TOKEN_WORD,
};

struct token {
    enum token_kind kind;
    const char *text; /* borrowed run inside the id; not NUL-terminated */
    size_t len;
};

/* Cut the next token from *cursor: a maximal run of digits or of letters, with every other
 * byte acting as a separator, so "qwen3.5-plus" and "claude-opus-4-5" tokenize alike. A run
 * of four or more digits reads as a snapshot date (a year, an MMDD tag, a YYYYMMDD stamp)
 * rather than a version. */
static struct token next_token(const char **cursor)
{
    const char *start = *cursor;
    while (*start && !isalnum((unsigned char)*start))
        start++;
    struct token token = {.kind = TOKEN_ID_END, .text = start, .len = 0};
    if (isdigit((unsigned char)*start)) {
        while (isdigit((unsigned char)start[token.len]))
            token.len++;
        token.kind = token.len >= 4 ? TOKEN_DATE : TOKEN_VERSION;
    } else {
        while (isalpha((unsigned char)start[token.len]))
            token.len++;
        if (token.len)
            token.kind = TOKEN_WORD;
    }
    *cursor = start + token.len;
    return token;
}

/* Numeric comparison without overflow: strip leading zeros, then longer means larger.
 * Larger sorts first — newest version or snapshot at the top. */
static int compare_digits_newest_first(const struct token *a, const struct token *b)
{
    const char *digits_a = a->text;
    const char *digits_b = b->text;
    size_t len_a = a->len;
    size_t len_b = b->len;
    while (len_a > 1 && *digits_a == '0') {
        digits_a++;
        len_a--;
    }
    while (len_b > 1 && *digits_b == '0') {
        digits_b++;
        len_b--;
    }
    if (len_a != len_b)
        return len_a > len_b ? -1 : 1;
    int cmp = memcmp(digits_a, digits_b, len_a);
    return cmp > 0 ? -1 : cmp < 0 ? 1 : 0;
}

static int compare_words(const struct token *a, const struct token *b)
{
    size_t common = a->len < b->len ? a->len : b->len;
    for (size_t i = 0; i < common; i++) {
        int char_a = tolower((unsigned char)a->text[i]);
        int char_b = tolower((unsigned char)b->text[i]);
        if (char_a != char_b)
            return char_a < char_b ? -1 : 1;
    }
    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    return 0;
}

int model_id_order(const char *a, const char *b)
{
    const char *cursor_a = a;
    const char *cursor_b = b;
    for (;;) {
        struct token token_a = next_token(&cursor_a);
        struct token token_b = next_token(&cursor_b);
        if (token_a.kind != token_b.kind)
            return token_a.kind < token_b.kind ? -1 : 1;
        if (token_a.kind == TOKEN_ID_END)
            return strcmp(a, b); /* punctuation-only differences still order deterministically */
        int cmp = token_a.kind == TOKEN_WORD ? compare_words(&token_a, &token_b)
                                             : compare_digits_newest_first(&token_a, &token_b);
        if (cmp)
            return cmp;
    }
}
