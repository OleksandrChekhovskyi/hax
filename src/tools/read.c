/* SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buf.h"
#include "provider.h"
#include "tool.h"
#include "util.h"
#include "system/fs.h"
#include "system/path.h"
#include "text/base64.h"
#include "text/utf8_sanitize.h"
#include "tools/image_sniff.h"
#include "tools/output_cap.h"
#include "tools/path_preprocess.h"

/* Raw ceiling for an attached image: 3/4 of the 5 MB base64 cap that is
 * the strictest common API limit (Anthropic rejects bigger payloads). */
#define READ_IMAGE_MAX_BYTES ((size_t)5 * 1024 * 1024 / 4 * 3)
/* Per-side pixel ceiling: Anthropic rejects above 8000px. The suggested
 * resize target is the token-optimal ~1568px, not this ceiling. */
#define READ_IMAGE_MAX_SIDE 8000L

enum read_truncation {
    READ_NOT_TRUNCATED,
    READ_TRUNCATED_BYTES,
    READ_TRUNCATED_LINES,
};

struct read_result {
    char *content; /* allocated; empty string when no content was read */
    size_t content_len;
    enum read_truncation truncation;
    int offset_past_eof;
    int is_binary;
    long line_count;
};

static int append_with_limit(struct buf *output, const char *chunk, size_t start, size_t end,
                             size_t output_limit)
{
    if (end <= start)
        return 0;
    if (output->len >= output_limit)
        return 1;

    size_t length = end - start;
    if (length > output_limit - output->len) {
        buf_append(output, chunk + start, output_limit - output->len);
        return 1;
    }
    buf_append(output, chunk + start, length);
    return 0;
}

static int append_numbered_run(struct buf *output, const char *chunk, size_t start, size_t end,
                               size_t output_limit, long line_number, int *needs_prefix)
{
    if (end <= start)
        return 0;

    if (*needs_prefix) {
        char prefix[24];
        int prefix_len = snprintf(prefix, sizeof(prefix), "%6ld" READ_LINE_DELIM, line_number);
        if (prefix_len < 0)
            prefix_len = 0;
        if ((size_t)prefix_len >= sizeof(prefix))
            prefix_len = (int)sizeof(prefix) - 1;

        /* A prefix without at least one content byte would look like file content. */
        if ((size_t)prefix_len >= output_limit - output->len)
            return 1;
        buf_append(output, prefix, (size_t)prefix_len);
        *needs_prefix = 0;
    }
    return append_with_limit(output, chunk, start, end, output_limit);
}

/* A read error after collecting a complete slice is conservatively treated as more content. */
static int stream_has_more_content(int fd, size_t consumed, size_t chunk_len)
{
    if (consumed < chunk_len)
        return 1;

    char byte;
    ssize_t bytes_read;
    do {
        bytes_read = read(fd, &byte, 1);
    } while (bytes_read < 0 && errno == EINTR);
    return bytes_read != 0;
}

/* Streams by fixed-size chunks so memory remains bounded even for a single enormous line.
 * Returns 0 on success and -1 with errno set on a read error. */
static int read_text_slice(const char *path, long offset, long limit, size_t output_limit,
                           struct read_result *result)
{
    result->content = NULL;
    result->content_len = 0;
    result->truncation = READ_NOT_TRUNCATED;
    result->offset_past_eof = 0;
    result->is_binary = 0;
    result->line_count = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    /* An explicit limit may tighten the shared line ceiling, but cannot raise it. */
    long line_limit = (long)OUTPUT_CAP_LINES;
    if (limit > 0 && limit < line_limit)
        line_limit = limit;

    struct buf output;
    buf_init(&output);
    char chunk[8192];
    long completed_lines = 0;
    long returned_lines = 0;
    long line_number = offset;
    enum read_truncation truncation = READ_NOT_TRUNCATED;
    int in_range = offset == 1;
    int reached_eof = 0;
    int current_line_has_content = 0;
    int checking_first_chunk = 1;
    int needs_prefix = in_range;

    for (;;) {
        ssize_t bytes_read = read(fd, chunk, sizeof(chunk));
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            int saved_errno = errno;
            close(fd);
            buf_free(&output);
            errno = saved_errno;
            return -1;
        }
        if (bytes_read == 0) {
            reached_eof = 1;
            break;
        }

        if (checking_first_chunk) {
            if (memchr(chunk, '\0', (size_t)bytes_read)) {
                result->is_binary = 1;
                close(fd);
                buf_free(&output);
                return 0;
            }
            checking_first_chunk = 0;
        }

        size_t run_start = 0;
        for (size_t i = 0; i < (size_t)bytes_read; i++) {
            char byte = chunk[i];
            if (byte != '\n')
                current_line_has_content = 1;

            if (!in_range) {
                if (byte == '\n') {
                    completed_lines++;
                    current_line_has_content = 0;
                    if (completed_lines + 1 >= offset) {
                        in_range = 1;
                        needs_prefix = 1;
                        run_start = i + 1;
                    }
                }
                continue;
            }

            if (byte != '\n')
                continue;

            if (append_numbered_run(&output, chunk, run_start, i + 1, output_limit, line_number,
                                    &needs_prefix)) {
                truncation = READ_TRUNCATED_BYTES;
                goto done;
            }
            run_start = i + 1;
            completed_lines++;
            returned_lines++;
            line_number++;
            needs_prefix = 1;
            current_line_has_content = 0;

            if (returned_lines < line_limit)
                continue;

            if (limit > 0 && limit <= (long)OUTPUT_CAP_LINES)
                goto done;
            if (stream_has_more_content(fd, i + 1, (size_t)bytes_read))
                truncation = READ_TRUNCATED_LINES;
            goto done;
        }

        if (in_range && append_numbered_run(&output, chunk, run_start, (size_t)bytes_read,
                                            output_limit, line_number, &needs_prefix)) {
            truncation = READ_TRUNCATED_BYTES;
            goto done;
        }
    }

done:
    close(fd);

    if (current_line_has_content && reached_eof) {
        completed_lines++;
        if (in_range)
            returned_lines++;
    }

    result->line_count = completed_lines;
    result->truncation = truncation;

    if (reached_eof && returned_lines == 0 && (completed_lines > 0 || offset > 1)) {
        result->offset_past_eof = 1;
        buf_free(&output);
        return 0;
    }

    if (!output.data) {
        result->content = xstrdup("");
        return 0;
    }
    result->content_len = output.len;
    result->content = buf_steal(&output);
    return 0;
}

static char *format_downscale_hint(const char *path)
{
    /* The suggested command may be run verbatim, so quote untrusted paths for the shell. */
    char *quoted_path = shell_single_quote(path);
    const char *path_prefix = path[0] == '-' ? "./" : "";

    char *executable_path = fs_which("magick");
    const char *command = NULL;
    if (executable_path) {
        command = "magick";
    } else {
        executable_path = fs_which("convert");
        if (executable_path)
            command = "convert"; /* ImageMagick 6 */
    }

    char *hint;
    if (command) {
        hint = xasprintf("downscale it first, e.g.: %s %s%s -resize '1568x1568>' "
                         "/tmp/downscaled.png — then read the copy",
                         command, path_prefix, quoted_path);
    } else if ((executable_path = fs_which("sips"))) {
        hint = xasprintf("downscale it first, e.g.: sips -Z 1568 %s%s "
                         "--out /tmp/downscaled.png — then read the copy",
                         path_prefix, quoted_path);
    } else {
        hint = xstrdup("downscale it first (no ImageMagick found on PATH; ask the user how "
                       "they'd like to resize it)");
    }

    free(executable_path);
    free(quoted_path);
    return hint;
}

static char *read_image(const char *path, size_t file_size, struct tool_run_ctx *ctx)
{
    if (!ctx || ctx->image_input == 0)
        return xasprintf("%s is an image, but the current model does not accept image input, "
                         "so it was not attached. Ask the user to switch to a vision-capable "
                         "model (or set image_input=on if this detection is wrong).",
                         path);

    if (file_size > READ_IMAGE_MAX_BYTES) {
        char *hint = format_downscale_hint(path);
        char *result = xasprintf("%s is %zu bytes; images over %zu bytes exceed provider "
                                 "limits — %s.",
                                 path, file_size, READ_IMAGE_MAX_BYTES, hint);
        free(hint);
        return result;
    }

    size_t image_len = 0;
    int truncated = 0;
    char *data = slurp_file_capped(path, READ_IMAGE_MAX_BYTES, &image_len, &truncated);
    if (!data || truncated) {
        free(data);
        return xasprintf("error reading %s: file changed while reading", path);
    }

    struct image_info info;
    if (!image_sniff(data, image_len, &info)) {
        free(data);
        return xasprintf("error reading %s: file changed while reading", path);
    }

    /* Undecodable attachments persist in history and would fail every subsequent turn. */
    if (info.width <= 0 || info.height <= 0 || !info.complete) {
        free(data);
        return xasprintf("%s looks like %s but is truncated or malformed, so it was not "
                         "attached. Check that the file is a complete image.",
                         path, info.mime);
    }

    if (info.width > READ_IMAGE_MAX_SIDE || info.height > READ_IMAGE_MAX_SIDE) {
        char *hint = format_downscale_hint(path);
        char *result = xasprintf("%s is %ldx%ld; images over %ldpx per side exceed provider "
                                 "limits — %s.",
                                 path, info.width, info.height, READ_IMAGE_MAX_SIDE, hint);
        free(hint);
        free(data);
        return result;
    }

    struct item_image *image = xcalloc(1, sizeof(*image));
    image->mime = xstrdup(info.mime);
    image->data_b64 = base64_encode(data, image_len, NULL);
    image->width = info.width;
    image->height = info.height;
    free(data);
    ctx->result_images = image;
    ctx->n_result_images = 1;

    /* Do not claim attachment: provider limits may still drop the image from model context. */
    return xasprintf("Read image %s (%s, %ldx%ld, %zu bytes).", path, info.mime, info.width,
                     info.height, image_len);
}

static int file_has_image_signature(const char *path)
{
    unsigned char header[16];
    ssize_t bytes_read;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    do {
        bytes_read = read(fd, header, sizeof(header));
    } while (bytes_read < 0 && errno == EINTR);
    close(fd);

    struct image_info info;
    return bytes_read > 0 && image_sniff(header, (size_t)bytes_read, &info);
}

static char *parse_line_argument(json_t *root, const char *name, long *value, int *provided)
{
    json_t *argument = json_object_get(root, name);
    *provided = argument != NULL;
    if (!argument)
        return NULL;
    if (!json_is_integer(argument))
        return xasprintf("'%s' must be an integer", name);

    *value = (long)json_integer_value(argument);
    if (*value < 1)
        return xasprintf("'%s' must be >= 1", name);
    return NULL;
}

/* Consumes `read_result->content` and returns allocated model-facing text. */
static char *format_text_result(struct read_result *read_result, long offset, size_t output_limit)
{
    if (read_result->offset_past_eof) {
        char *result =
            xasprintf("(file has %ld line%s; offset %ld is past EOF)", read_result->line_count,
                      read_result->line_count == 1 ? "" : "s", offset);
        free(read_result->content);
        return result;
    }

    /* Cap lines before sanitizing so the generated elision markers remain valid UTF-8. */
    size_t capped_len = 0;
    char *capped = cap_line_lengths(read_result->content, read_result->content_len,
                                    OUTPUT_CAP_LINE_WIDTH, &capped_len);
    free(read_result->content);

    char *content = utf8_sanitize(capped, capped_len);
    free(capped);

    if (read_result->truncation == READ_TRUNCATED_BYTES) {
        char *result =
            xasprintf("%s\n\n[truncated at %zu bytes; file is larger — pass offset/limit "
                      "to read more]",
                      content, output_limit);
        free(content);
        return result;
    }
    if (read_result->truncation == READ_TRUNCATED_LINES) {
        char *result = xasprintf("%s\n\n[truncated at %d lines; file has more — pass offset/limit "
                                 "to read more]",
                                 content, OUTPUT_CAP_LINES);
        free(content);
        return result;
    }
    return content;
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
    if (!raw_path || !*raw_path) {
        result = xstrdup("missing 'path' argument");
        goto out;
    }

    long offset = 1;
    long limit = 0;
    int offset_provided;
    int limit_provided;
    result = parse_line_argument(root, "offset", &offset, &offset_provided);
    if (result)
        goto out;
    result = parse_line_argument(root, "limit", &limit, &limit_provided);
    if (result)
        goto out;

    path = path_expand_home(raw_path);
    struct stat st;
    if (stat(path, &st) < 0) {
        result = xasprintf("error reading %s: %s", path, strerror(errno));
        goto out;
    }
    /* Opening a FIFO without a writer can block indefinitely. */
    if (!S_ISREG(st.st_mode)) {
        result = xasprintf("%s exists but is not a regular file", path);
        goto out;
    }

    /* Image signatures must be checked before the text reader's binary-file rejection. */
    if (file_has_image_signature(path)) {
        result = read_image(path, (size_t)st.st_size, ctx);
        goto out;
    }

    size_t output_limit = output_cap_bytes();
    if (!offset_provided && !limit_provided && (size_t)st.st_size > output_limit) {
        result = xasprintf("%s is %lld bytes; cap is %zu. Pass offset/limit to read a slice, "
                           "or use bash with grep/head/tail.",
                           path, (long long)st.st_size, output_limit);
        goto out;
    }

    struct read_result read_result;
    if (read_text_slice(path, offset, limit, output_limit, &read_result) < 0) {
        result = xasprintf("error reading %s: %s", path, strerror(errno));
        goto out;
    }
    if (read_result.is_binary) {
        result = xasprintf("%s appears to be binary (NUL byte found in first 8 KiB)", path);
        free(read_result.content);
        goto out;
    }
    result = format_text_result(&read_result, offset, output_limit);

out:
    free(path);
    json_decref(root);
    return result;
}

static int path_has_image_extension(const char *path)
{
    const char *extension = path ? strrchr(path, '.') : NULL;
    if (!extension)
        return 0;
    static const char *const extensions[] = {".png", ".jpg", ".jpeg", ".gif", ".webp"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
        if (strcasecmp(extension, extensions[i]) == 0)
            return 1;
    return 0;
}

static char *format_line_range(const char *args_json)
{
    if (!args_json)
        return NULL;

    json_error_t json_error;
    json_t *root = json_loads(args_json, 0, &json_error);
    if (!root)
        return NULL;

    json_t *offset_json = json_object_get(root, "offset");
    json_t *limit_json = json_object_get(root, "limit");
    char *range = NULL;
    int has_range = offset_json || limit_json;
    const char *path = json_string_value(json_object_get(root, "path"));
    if (has_range && !path_has_image_extension(path)) {
        long offset = json_is_integer(offset_json) ? (long)json_integer_value(offset_json) : 1;
        if (json_is_integer(limit_json)) {
            long limit = (long)json_integer_value(limit_json);
            if (limit < 1) {
                range = xasprintf(":%ld-", offset);
            } else {
                /* Tool arguments are untrusted; clamp rather than overflowing the range end. */
                long end = offset > LONG_MAX - limit + 1 ? LONG_MAX : offset + limit - 1;
                range = xasprintf(":%ld-%ld", offset, end);
            }
        } else {
            range = xasprintf(":%ld-", offset);
        }
    }
    json_decref(root);
    return range;
}

/* Trailing slashes retain the full path because their basename is empty. */
static const char *basename_view(const char *path)
{
    if (!path || !*path)
        return "?";
    const char *slash = strrchr(path, '/');
    if (!slash || slash[1] == '\0')
        return path;
    return slash + 1;
}

/* Filenames that recur across directories by convention, so the basename alone does not
 * identify the file. All-uppercase stems (README, LICENSE, SKILL, ...) and the stems "index"
 * and "main" are matched by rule rather than listed. */
/* clang-format off */
static const char *const GENERIC_BASENAMES[] = {
    "AndroidManifest.xml", "build.gradle", "build.gradle.kts", "build.rs", "build.zig",
    "Cargo.toml", "Chart.yaml", "CMakeLists.txt", "compose.yaml", "compose.yml",
    "composer.json", "configure.ac", "conftest.py", "default.nix", "docker-compose.yaml",
    "docker-compose.yml", "Dockerfile", "flake.nix", "Gemfile", "GNUmakefile", "go.mod",
    "go.sum", "__init__.py", "Jenkinsfile", "justfile", "Kbuild", "Kconfig",
    "kustomization.yaml", "lib.rs", "__main__.py", "Makefile", "manifest.json", "meson.build",
    "mix.exs", "mod.rs", "outputs.tf", "Package.swift", "package.json", "Podfile", "pom.xml",
    "project.pbxproj", "pyproject.toml", "Rakefile", "requirements.txt", "settings.gradle",
    "settings.gradle.kts", "setup.py", "shell.nix", "tsconfig.json", "values.yaml",
    "variables.tf", "versions.tf",
};
/* clang-format on */

static int stem_is_all_caps(const char *name)
{
    int has_upper = 0;
    for (const char *c = name; *c && *c != '.'; c++) {
        if (islower((unsigned char)*c))
            return 0;
        if (isupper((unsigned char)*c))
            has_upper = 1;
    }
    return has_upper;
}

static int stem_equals(const char *name, const char *stem)
{
    size_t stem_len = strlen(stem);
    return strncasecmp(name, stem, stem_len) == 0 &&
           (name[stem_len] == '\0' || name[stem_len] == '.');
}

static int basename_is_generic(const char *name)
{
    if (name[0] == '.' || stem_is_all_caps(name) || stem_equals(name, "index") ||
        stem_equals(name, "main"))
        return 1;
    for (size_t i = 0; i < sizeof(GENERIC_BASENAMES) / sizeof(GENERIC_BASENAMES[0]); i++)
        if (strcasecmp(name, GENERIC_BASENAMES[i]) == 0)
            return 1;
    return 0;
}

/* Collapsed rows show the basename alone, except for convention names whose identity lives
 * in the directory: those keep one parent component. */
static char *collapse_path(const char *path)
{
    const char *base = basename_view(path);
    if (base == path || !basename_is_generic(base))
        return xstrdup(base);

    const char *slash = base - 1;
    const char *parent = slash;
    while (parent > path && parent[-1] != '/')
        parent--;
    if (parent == slash)
        return xstrdup(base);

    /* An untouched or root-anchored prefix is complete; anything shorter was elided. */
    if (parent == path || (parent == path + 1 && path[0] == '/'))
        return xstrdup(path);
    return xasprintf(".../%s", parent);
}

static const char READ_DESCRIPTION[] =
    "Read a file from disk and return its contents in `cat -n` style: each line is prefixed with "
    "its 1-indexed line number, a " READ_LINE_DELIM " arrow, then the line's content. The prefix "
    "is presentation only — it is NOT part of the file on disk; do not include it in `edit` tool "
    "`old_string`/`new_string` arguments. Optional 1-indexed line `offset` and `limit` slice a "
    "range; without them, the whole file is returned. Image files (PNG/JPEG/GIF/WebP) are "
    "detected by content and attached to the result as images when the model supports image "
    "input.";

static const struct tool_param READ_PARAMS[] = {
    {.name = "path", .type = "string", .required = 1, .description = "Path to the file."},
    {.name = "offset",
     .type = "integer",
     .minimum = 1,
     .description = "1-indexed first line to return. Default 1."},
    {.name = "limit",
     .type = "integer",
     .minimum = 1,
     .description = "Maximum number of lines to return. Default: to EOF."},
};

const struct tool TOOL_READ = {
    .def = {.name = "read",
            .description = READ_DESCRIPTION,
            .params = READ_PARAMS,
            .n_params = sizeof(READ_PARAMS) / sizeof(READ_PARAMS[0])},
    .run = run,
    .preprocess_args = tool_relativize_path_args,
    .display = {.arg_name = "path",
                .format_extra = format_line_range,
                .preview_mode = TOOL_PREVIEW_COLLAPSED,
                .collapse_argument = collapse_path},
};
