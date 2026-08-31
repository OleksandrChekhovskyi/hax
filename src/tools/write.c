/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stddef.h>
#include <stdlib.h>

#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "system/fs.h"
#include "system/path.h"
#include "tools/path_preprocess.h"

static size_t count_lines(const char *content, size_t content_len)
{
    if (content_len == 0)
        return 0;

    size_t line_count = content[content_len - 1] == '\n' ? 0 : 1;
    for (size_t i = 0; i < content_len; i++) {
        if (content[i] == '\n')
            line_count++;
    }
    return line_count;
}

static char *run(const char *args_json, struct tool_run_ctx *ctx)
{
    json_error_t json_error;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &json_error);
    if (!root)
        return xasprintf("invalid arguments: %s", json_error.text);

    char *result = NULL;
    char *path = NULL;
    const char *raw_path = json_string_value(json_object_get(root, "path"));
    json_t *content_json = json_object_get(root, "content");
    if (!raw_path || !*raw_path) {
        result = xstrdup("missing 'path' argument");
        goto out;
    }
    if (!json_is_string(content_json)) {
        result = xstrdup("missing 'content' argument");
        goto out;
    }

    path = path_expand_home(raw_path);
    const char *content = json_string_value(content_json);
    size_t content_len = json_string_length(content_json);

    char *error = NULL;
    int created = 0;
    result = fs_write_with_diff(path, content, content_len, &error, &created);
    if (error) {
        free(result);
        result = error;
        goto out;
    }

    if (created) {
        free(result);
        /* The streamed preview already carries new-file content in the user-facing history. */
        if (ctx && ctx->display && content_len > 0) {
            ctx->display(content, content_len, ctx->display_data);
            ctx->output_summarizes_display = 1;
        }

        size_t line_count = count_lines(content, content_len);
        if (content_len == 0) {
            result = xasprintf("created %s (empty)", path);
        } else {
            result =
                xasprintf("created %s (%zu line%s, %zu byte%s)", path, line_count,
                          line_count == 1 ? "" : "s", content_len, content_len == 1 ? "" : "s");
        }
    }

out:
    free(path);
    json_decref(root);
    return result;
}

static const char WRITE_DESCRIPTION[] =
    "Write a file, replacing it entirely (creating it if needed). Parent directories are created "
    "automatically.";

static const struct tool_param WRITE_PARAMS[] = {
    {.name = "path", .type = "string", .required = 1, .description = "Path to the file."},
    {.name = "content",
     .type = "string",
     .required = 1,
     .description = "Full new contents of the file."},
};

const struct tool TOOL_WRITE = {
    .def = {.name = "write",
            .description = WRITE_DESCRIPTION,
            .params = WRITE_PARAMS,
            .n_params = sizeof(WRITE_PARAMS) / sizeof(WRITE_PARAMS[0])},
    .run = run,
    .preprocess_args = tool_relativize_path_args,
    .display = {.arg_name = "path", .output_style = TOOL_OUTPUT_UNIFIED_DIFF},
};
