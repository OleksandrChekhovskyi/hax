/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <jansson.h>

#include "fs.h"
#include "util.h"

static char *run(const char *args_json)
{
    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *path = json_string_value(json_object_get(root, "path"));
    json_t *jc = json_object_get(root, "content");
    if (!path || !*path) {
        json_decref(root);
        return xstrdup("missing 'path' argument");
    }
    if (!jc || !json_is_string(jc)) {
        json_decref(root);
        return xstrdup("missing 'content' argument");
    }
    const char *content = json_string_value(jc);
    size_t content_len = json_string_length(jc);

    char *errmsg = NULL;
    char *diff = fs_write_with_diff(path, content, content_len, &errmsg);
    json_decref(root);

    if (errmsg) {
        free(diff);
        return errmsg;
    }
    return diff;
}

const struct tool TOOL_WRITE = {
    .def =
        {
            .name = "write",
            .description = "Write a file, replacing it entirely (creating it if needed). "
                           "Parent directories are created automatically. Returns a "
                           "unified diff of the change.",
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
