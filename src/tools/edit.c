/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <jansson.h>
#include <string.h>
#include <sys/stat.h>

#include "fs.h"
#include "util.h"

#define EDIT_READ_CAP (4 * 1024 * 1024)

/* Count non-overlapping occurrences of needle in [hay, hay+hay_len). After
 * a match we skip past needle_len bytes — this matches the semantics of
 * the replacement loop, so the count accurately reflects how many replaces
 * would happen with replace_all. */
static size_t count_occurrences(const char *hay, size_t hay_len, const char *needle,
                                size_t needle_len)
{
    if (needle_len == 0 || needle_len > hay_len)
        return 0;
    size_t n = 0;
    size_t i = 0;
    while (i + needle_len <= hay_len) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            n++;
            i += needle_len;
        } else {
            i++;
        }
    }
    return n;
}

/* Replace occurrences of old_str with new_str in hay. With only_first set,
 * stops after the first match; otherwise replaces all non-overlapping
 * matches. Caller frees the returned buffer; *out_len receives the byte
 * count. */
static char *do_replace(const char *hay, size_t hay_len, const char *old_str, size_t old_len,
                        const char *new_str, size_t new_len, int only_first, size_t *out_len)
{
    struct buf out;
    buf_init(&out);
    size_t i = 0;
    int replaced = 0;
    while (i < hay_len) {
        if (i + old_len <= hay_len && (!only_first || !replaced) &&
            memcmp(hay + i, old_str, old_len) == 0) {
            buf_append(&out, new_str, new_len);
            i += old_len;
            replaced = 1;
            if (only_first) {
                buf_append(&out, hay + i, hay_len - i);
                i = hay_len;
            }
        } else {
            buf_append(&out, hay + i, 1);
            i++;
        }
    }
    *out_len = out.len;
    if (!out.data)
        return xstrdup("");
    return buf_steal(&out);
}

static char *run(const char *args_json)
{
    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *path = json_string_value(json_object_get(root, "path"));
    json_t *jold = json_object_get(root, "old_string");
    json_t *jnew = json_object_get(root, "new_string");
    json_t *jra = json_object_get(root, "replace_all");

    if (!path || !*path) {
        json_decref(root);
        return xstrdup("missing 'path' argument");
    }
    if (!jold || !json_is_string(jold)) {
        json_decref(root);
        return xstrdup("missing 'old_string' argument");
    }
    if (!jnew || !json_is_string(jnew)) {
        json_decref(root);
        return xstrdup("missing 'new_string' argument");
    }
    const char *old_s = json_string_value(jold);
    size_t old_len = json_string_length(jold);
    const char *new_s = json_string_value(jnew);
    size_t new_len = json_string_length(jnew);
    int replace_all = jra && json_is_true(jra);

    if (old_len == 0) {
        json_decref(root);
        return xstrdup("'old_string' must be non-empty");
    }
    if (old_len == new_len && memcmp(old_s, new_s, old_len) == 0) {
        json_decref(root);
        return xstrdup("'old_string' and 'new_string' are identical — nothing to do");
    }

    /* Refuse FIFOs, sockets, devices, directories before slurping.
     * slurp_file_capped's internal guard would also reject these, but
     * we surface a tool-specific "not a regular file" error instead of
     * the bare "Invalid argument" that would otherwise reach the model.
     * Mirrors the same guard inside fs_write_with_diff. stat() follows
     * symlinks, so a link to a regular file still passes. */
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
        char *msg = xasprintf("%s exists but is not a regular file", path);
        json_decref(root);
        return msg;
    }

    size_t orig_len = 0;
    int truncated = 0;
    char *orig = slurp_file_capped(path, EDIT_READ_CAP, &orig_len, &truncated);
    if (!orig) {
        char *msg = xasprintf("error reading %s: %s", path, strerror(errno));
        json_decref(root);
        return msg;
    }
    if (truncated) {
        char *msg =
            xasprintf("file %s is larger than %d bytes — refusing to edit", path, EDIT_READ_CAP);
        free(orig);
        json_decref(root);
        return msg;
    }

    size_t n_matches = count_occurrences(orig, orig_len, old_s, old_len);
    if (n_matches == 0) {
        free(orig);
        json_decref(root);
        return xstrdup("'old_string' not found in file");
    }
    if (n_matches > 1 && !replace_all) {
        char *msg = xasprintf("'old_string' matches %zu places in %s — provide more context "
                              "to disambiguate, or set replace_all=true",
                              n_matches, path);
        free(orig);
        json_decref(root);
        return msg;
    }

    size_t new_total_len = 0;
    char *updated =
        do_replace(orig, orig_len, old_s, old_len, new_s, new_len, !replace_all, &new_total_len);
    free(orig);

    char *errmsg = NULL;
    char *diff = fs_write_with_diff(path, updated, new_total_len, &errmsg);
    free(updated);
    json_decref(root);

    if (errmsg) {
        free(diff);
        return errmsg;
    }
    return diff;
}

const struct tool TOOL_EDIT = {
    .def =
        {
            .name = "edit",
            .description = "Replace an exact string in a file. The `old_string` must match a "
                           "byte sequence in the file exactly once unless `replace_all` is "
                           "true. Returns a unified diff of the change.",
            .parameters_schema_json =
                "{\"type\":\"object\","
                "\"properties\":{"
                "\"path\":{\"type\":\"string\","
                "\"description\":\"Path to the file.\"},"
                "\"old_string\":{\"type\":\"string\","
                "\"description\":\"Exact text to find. Must be unique unless replace_all is "
                "set.\"},"
                "\"new_string\":{\"type\":\"string\","
                "\"description\":\"Replacement text.\"},"
                "\"replace_all\":{\"type\":\"boolean\","
                "\"description\":\"Replace every occurrence instead of requiring uniqueness.\"}"
                "},"
                "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
            .display_arg = "path",
        },
    .run = run,
    .output_is_diff = 1,
};
