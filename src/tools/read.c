/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <jansson.h>
#include <string.h>

#include "util.h"

#define READ_CAP (256 * 1024)

static char *run(const char *args_json)
{
    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *path = json_string_value(json_object_get(root, "path"));
    if (!path || !*path) {
        json_decref(root);
        return xstrdup("missing 'path' argument");
    }

    size_t got = 0;
    int truncated = 0;
    char *raw = slurp_file_capped(path, READ_CAP, &got, &truncated);
    if (!raw) {
        char *msg = xasprintf("error reading %s: %s", path, strerror(errno));
        json_decref(root);
        return msg;
    }

    char *clean = sanitize_utf8(raw, got);
    free(raw);
    json_decref(root);

    if (truncated) {
        char *msg = xasprintf("%s\n\n[truncated at %d bytes; file is larger]", clean, READ_CAP);
        free(clean);
        return msg;
    }
    return clean;
}

const struct tool TOOL_READ = {
    .def =
        {
            .name = "read",
            .description = "Read a file from disk and return its contents.",
            .parameters_schema_json = "{\"type\":\"object\","
                                      "\"properties\":{\"path\":{\"type\":\"string\","
                                      "\"description\":\"Path to the file.\"}},"
                                      "\"required\":[\"path\"]}",
            .display_arg = "path",
        },
    .run = run,
};
