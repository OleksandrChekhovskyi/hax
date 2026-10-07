/* SPDX-License-Identifier: MIT */
#include "text/json_scan.h"

#include <jansson.h>
#include <string.h>

/* The byte scan is UTF-8-safe because quotes and backslashes cannot occur inside multibyte
 * sequences. */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Advance past the string whose opening quote is at `p`. Returns NULL on truncated input. */
static const char *skip_string(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!p[1])
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Advance past one value, honoring strings and bracket nesting; scalars run to the next delimiter.
 * Returns NULL on truncated input. */
static const char *skip_value(const char *p)
{
    if (*p == '"')
        return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_string(p);
                if (!p)
                    return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' &&
           *p != '\r')
        p++;
    return p;
}

static int scan_open(struct json_scan *scan, const char *text, char open, char close)
{
    const char *p = skip_ws(text);
    if (*p != open)
        return -1;
    p = skip_ws(p + 1);
    scan->close = close;
    scan->done = *p == close;
    scan->cursor = scan->done ? p + 1 : p;
    return 0;
}

int json_scan_object(struct json_scan *scan, const char *text)
{
    return scan_open(scan, text, '{', '}');
}

int json_scan_array(struct json_scan *scan, const char *text)
{
    return scan_open(scan, text, '[', ']');
}

int json_scan_next(struct json_scan *scan, struct json_scan_entry *entry)
{
    if (scan->done)
        return 0;

    const char *p = scan->cursor;
    entry->key = NULL;
    entry->key_len = 0;
    if (scan->close == '}') {
        if (*p != '"')
            return -1;
        const char *key_end = skip_string(p);
        if (!key_end)
            return -1;
        entry->key = p + 1;
        entry->key_len = (size_t)(key_end - 1 - entry->key);
        p = skip_ws(key_end);
        if (*p != ':')
            return -1;
        p = skip_ws(p + 1);
    }

    const char *value_end = skip_value(p);
    if (!value_end || value_end == p)
        return -1;
    entry->value = p;
    entry->value_len = (size_t)(value_end - p);

    p = skip_ws(value_end);
    if (*p == ',') {
        scan->cursor = skip_ws(p + 1);
    } else if (*p == scan->close) {
        scan->cursor = p + 1;
        scan->done = 1;
    } else {
        return -1;
    }
    return 1;
}

int json_scan_find(const char *text, const char *key, struct json_scan_entry *member)
{
    struct json_scan scan;
    if (json_scan_object(&scan, text) != 0)
        return -1;

    size_t key_len = strlen(key);
    int result;
    while ((result = json_scan_next(&scan, member)) == 1) {
        if (member->key_len == key_len && memcmp(member->key, key, key_len) == 0)
            return 1;
    }
    return result;
}

json_t *json_scan_load(const struct json_scan_entry *entry)
{
    return json_loadb(entry->value, entry->value_len, JSON_DECODE_ANY, NULL);
}
