/* SPDX-License-Identifier: MIT */
#include <jansson.h>

#include "tool.h"
#include "util.h"
#include "system/fs.h"
#include "system/path.h"

/* Count '\n' bytes plus a trailing partial line (content not ending in
 * '\n' still counts as one line — matches what `wc -l` would feel intuitive
 * for, even though wc itself only counts terminators). Empty content is
 * 0 lines. */
static size_t count_lines(const char *s, size_t n)
{
    size_t lines = 0;
    int saw_data = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            lines++;
            saw_data = 0;
        } else {
            saw_data = 1;
        }
    }
    if (saw_data)
        lines++;
    return lines;
}

static char *run(const char *args_json, tool_emit_display_fn emit_display, void *user)
{
    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *raw_path = json_string_value(json_object_get(root, "path"));
    json_t *jc = json_object_get(root, "content");
    if (!raw_path || !*raw_path) {
        json_decref(root);
        return xstrdup("missing 'path' argument");
    }
    if (!jc || !json_is_string(jc)) {
        json_decref(root);
        return xstrdup("missing 'content' argument");
    }
    char *path = expand_home(raw_path);
    const char *content = json_string_value(jc);
    size_t content_len = json_string_length(jc);

    char *errmsg = NULL;
    int was_new = 0;
    char *diff = fs_write_with_diff(path, content, content_len, &errmsg, &was_new);

    if (errmsg) {
        free(diff);
        free(path);
        json_decref(root);
        return errmsg;
    }

    /* For new files the unified diff is just the content again with `+`
     * prefixes — wasteful in history since the model already sent the
     * bytes in this call's arguments. Push the content through the
     * display-only emit_display channel (see tool.h) so the user still gets a
     * head-capped dim preview, and return a short confirmation as the
     * canonical output. */
    if (was_new) {
        free(diff);
        if (emit_display && content_len > 0)
            emit_display(content, content_len, user);
        char *result;
        size_t lines = count_lines(content, content_len);
        if (content_len == 0)
            result = xasprintf("created %s (empty)", path);
        else
            result = xasprintf("created %s (%zu line%s, %zu byte%s)", path, lines,
                               lines == 1 ? "" : "s", content_len, content_len == 1 ? "" : "s");
        free(path);
        json_decref(root);
        return result;
    }

    free(path);
    json_decref(root);
    return diff;
}

const struct tool TOOL_WRITE = {
    .def =
        {
            .name = "write",
            .description = "Write a file, replacing it entirely (creating it if needed). "
                           "Parent directories are created automatically.",
            .parameters_schema_json = "{\"type\":\"object\","
                                      "\"properties\":{"
                                      "\"path\":{\"type\":\"string\","
                                      "\"description\":\"Path to the file.\"},"
                                      "\"content\":{\"type\":\"string\","
                                      "\"description\":\"Full new contents of the file.\"}"
                                      "},"
                                      "\"required\":[\"path\",\"content\"]}",
            .display_arg = "path",
        },
    .run = run,
    .output_is_diff = 1,
};
