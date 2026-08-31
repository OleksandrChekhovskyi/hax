/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "buf.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "system/fs.h"
#include "system/path.h"
#include "tools/output_cap.h"
#include "tools/path_preprocess.h"

#define EDIT_READ_CAP (4 * 1024 * 1024)

static size_t count_occurrences(const char *content, size_t content_len, const char *search,
                                size_t search_len)
{
    if (search_len == 0 || search_len > content_len)
        return 0;

    size_t count = 0;
    size_t offset = 0;
    while (offset + search_len <= content_len) {
        if (memcmp(content + offset, search, search_len) == 0) {
            count++;
            offset += search_len;
        } else {
            offset++;
        }
    }
    return count;
}

static char *replace_occurrences(const char *content, size_t content_len, const char *search,
                                 size_t search_len, const char *replacement, size_t replacement_len,
                                 size_t *result_len)
{
    struct buf result;
    buf_init(&result);

    size_t offset = 0;
    size_t unchanged_start = 0;
    while (offset + search_len <= content_len) {
        if (memcmp(content + offset, search, search_len) != 0) {
            offset++;
            continue;
        }

        buf_append(&result, content + unchanged_start, offset - unchanged_start);
        buf_append(&result, replacement, replacement_len);
        offset += search_len;
        unchanged_start = offset;
    }
    buf_append(&result, content + unchanged_start, content_len - unchanged_start);

    *result_len = result.len;
    return result.data ? buf_steal(&result) : xstrdup("");
}

static char *run(const char *args_json, struct tool_run_ctx *ctx)
{
    (void)ctx;
    json_error_t json_error;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &json_error);
    if (!root)
        return xasprintf("invalid arguments: %s", json_error.text);

    char *result = NULL;
    char *path = NULL;
    char *original = NULL;
    char *updated = NULL;

    const char *raw_path = json_string_value(json_object_get(root, "path"));
    json_t *old_string_json = json_object_get(root, "old_string");
    json_t *new_string_json = json_object_get(root, "new_string");
    json_t *replace_all_json = json_object_get(root, "replace_all");

    if (!raw_path || !*raw_path) {
        result = xstrdup("missing 'path' argument");
        goto out;
    }
    if (!json_is_string(old_string_json)) {
        result = xstrdup("missing 'old_string' argument");
        goto out;
    }
    if (!json_is_string(new_string_json)) {
        result = xstrdup("missing 'new_string' argument");
        goto out;
    }

    const char *old_string = json_string_value(old_string_json);
    size_t old_string_len = json_string_length(old_string_json);
    const char *new_string = json_string_value(new_string_json);
    size_t new_string_len = json_string_length(new_string_json);
    int replace_all = json_is_true(replace_all_json);

    if (old_string_len == 0) {
        result = xstrdup("'old_string' must be non-empty");
        goto out;
    }
    if (old_string_len == new_string_len && memcmp(old_string, new_string, old_string_len) == 0) {
        result = xstrdup("'old_string' and 'new_string' are identical — nothing to do");
        goto out;
    }

    path = path_expand_home(raw_path);

    /* Avoid blocking on FIFOs and replacing special files with regular files. */
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
        result = xasprintf("%s exists but is not a regular file", path);
        goto out;
    }

    size_t original_len = 0;
    int truncated = 0;
    original = fs_read_file_capped(path, EDIT_READ_CAP, &original_len, &truncated);
    if (!original) {
        result = xasprintf("error reading %s: %s", path, strerror(errno));
        goto out;
    }
    if (truncated) {
        result =
            xasprintf("file %s is larger than %d bytes — refusing to edit", path, EDIT_READ_CAP);
        goto out;
    }

    size_t match_count = count_occurrences(original, original_len, old_string, old_string_len);
    if (match_count == 0) {
        result = xstrdup("'old_string' not found in file");
        goto out;
    }
    if (match_count > 1 && !replace_all) {
        result = xasprintf("'old_string' matches %zu places in %s — provide more context "
                           "to disambiguate, or set replace_all=true",
                           match_count, path);
        goto out;
    }

    size_t updated_len = 0;
    updated = replace_occurrences(original, original_len, old_string, old_string_len, new_string,
                                  new_string_len, &updated_len);

    char *error = NULL;
    result = fs_write_with_diff(path, updated, updated_len, &error, NULL);
    if (error) {
        free(result);
        result = error;
    }

out:
    free(updated);
    free(original);
    free(path);
    json_decref(root);
    return result;
}

static const char EDIT_DESCRIPTION[] =
    "Replace an exact string in a file. The `old_string` must match a byte sequence in the file "
    "exactly once unless `replace_all` is true. The `read` tool prefixes each line with a line "
    "number and a " READ_LINE_DELIM " arrow for display; that prefix is NOT part of the file on "
    "disk, so do not include it in `old_string` or `new_string`. Returns a unified diff of the "
    "change.";

static const struct tool_param EDIT_PARAMS[] = {
    {.name = "path", .type = "string", .required = 1, .description = "Path to the file."},
    {.name = "old_string",
     .type = "string",
     .required = 1,
     .description = "Exact text to find. Must be unique unless replace_all is set."},
    {.name = "new_string", .type = "string", .required = 1, .description = "Replacement text."},
    {.name = "replace_all",
     .type = "boolean",
     .description = "Replace every occurrence instead of requiring uniqueness."},
};

const struct tool TOOL_EDIT = {
    .def = {.name = "edit",
            .description = EDIT_DESCRIPTION,
            .params = EDIT_PARAMS,
            .n_params = sizeof(EDIT_PARAMS) / sizeof(EDIT_PARAMS[0])},
    .run = run,
    .preprocess_args = tool_relativize_path_args,
    .display = {.arg_name = "path", .output_style = TOOL_OUTPUT_UNIFIED_DIFF},
};
