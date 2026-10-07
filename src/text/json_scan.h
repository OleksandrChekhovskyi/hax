/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_JSON_SCAN_H
#define HAX_TEXT_JSON_SCAN_H

#include <jansson.h>
#include <stddef.h>

/* Walk the members of a JSON object or the elements of an array without tree-parsing the whole
 * text, since Jansson's tree is several times the size of the text. The scan follows only strings
 * and brackets, so a scanned value is validated when it is loaded. */

struct json_scan {
    const char *cursor; /* next entry; past the closing bracket once done */
    char close;         /* '}' or ']' */
    int done;
};

/* A borrowed span of the scanned text. `key` is set for object members only and keeps its escape
 * sequences. */
struct json_scan_entry {
    const char *key;
    size_t key_len;
    const char *value;
    size_t value_len;
};

/* Start scanning the object or array that `text` begins with, after whitespace. Returns 0, or -1
 * when it begins with something else. */
int json_scan_object(struct json_scan *scan, const char *text);
int json_scan_array(struct json_scan *scan, const char *text);

/* Returns 1 with `entry` filled, 0 once past the last entry, or -1 on malformed or truncated
 * text. */
int json_scan_next(struct json_scan *scan, struct json_scan_entry *entry);

/* Find the member of object `text` whose raw key is `key`. Returns 1 with `member` filled, 0 when
 * absent, or -1 when `text` is not a well-formed object up to the member. */
int json_scan_find(const char *text, const char *key, struct json_scan_entry *member);

/* Parse an entry's value. Returns a new reference, or NULL when the value is invalid. */
json_t *json_scan_load(const struct json_scan_entry *entry);

#endif /* HAX_TEXT_JSON_SCAN_H */
