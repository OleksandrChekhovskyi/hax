/* SPDX-License-Identifier: MIT */
#include "providers/codex_settings.h"

#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "system/fs.h"
#include "system/path.h"

static const char *skip_inline_whitespace(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
        cursor++;
    return cursor;
}

static int toml_key_matches(const char *cursor, const char *end, const char *key)
{
    size_t key_len = strlen(key);
    if ((size_t)(end - cursor) < key_len || memcmp(cursor, key, key_len) != 0)
        return 0;

    cursor += key_len;
    return cursor == end || *cursor == '=' || *cursor == ' ' || *cursor == '\t';
}

static char *parse_toml_string(const char *value, const char *end)
{
    if (value >= end || (*value != '"' && *value != '\''))
        return NULL;

    char quote = *value++;
    struct buf result;
    buf_init(&result);

    while (value < end) {
        char byte = *value++;
        if (byte == quote)
            return buf_steal(&result);

        if (quote == '"' && byte == '\\' && value < end) {
            byte = *value++;
            switch (byte) {
            case 'b':
                byte = '\b';
                break;
            case 't':
                byte = '\t';
                break;
            case 'n':
                byte = '\n';
                break;
            case 'f':
                byte = '\f';
                break;
            case 'r':
                byte = '\r';
                break;
            case '"':
            case '\\':
                break;
            default:
                /* Preserve unsupported escaped bytes instead of rejecting otherwise usable Codex
                 * settings. */
                break;
            }
        }
        buf_append(&result, &byte, 1);
    }

    buf_free(&result);
    return NULL;
}

char *codex_toml_top_level_string(const char *contents, size_t contents_len, const char *key)
{
    const char *cursor = contents;
    const char *end = contents + contents_len;

    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (!line_end)
            line_end = end;

        const char *assignment = skip_inline_whitespace(cursor, line_end);
        if (assignment < line_end && *assignment == '[')
            return NULL;
        if (assignment < line_end && *assignment != '#' &&
            toml_key_matches(assignment, line_end, key)) {
            assignment = skip_inline_whitespace(assignment + strlen(key), line_end);
            if (assignment < line_end && *assignment == '=') {
                assignment = skip_inline_whitespace(assignment + 1, line_end);
                return parse_toml_string(assignment, line_end);
            }
        }

        cursor = line_end < end ? line_end + 1 : end;
    }

    return NULL;
}

static char *read_setting(const char *contents, size_t contents_len, const char *key)
{
    char *value = codex_toml_top_level_string(contents, contents_len, key);
    if (value && !*value) {
        free(value);
        return NULL;
    }
    return value;
}

void codex_load_settings(char **model, char **effort)
{
    *model = NULL;
    *effort = NULL;

    char *path = path_expand_home("~/.codex/config.toml");
    size_t contents_len = 0;
    char *contents = fs_read_file(path, &contents_len);
    free(path);
    if (!contents)
        return;

    *model = read_setting(contents, contents_len, "model");
    *effort = read_setting(contents, contents_len, "model_reasoning_effort");
    free(contents);
}
