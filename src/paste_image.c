/* SPDX-License-Identifier: MIT */
#include "paste_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "buf.h"
#include "util.h"
#include "system/fs.h"
#include "system/tempfiles.h"
#include "terminal/clipboard.h"
#include "tools/image_sniff.h"

size_t paste_image_normalize_text(char *text, size_t text_len)
{
    size_t write_offset = 0;
    for (size_t i = 0; i < text_len; i++) {
        char byte = text[i];
        if (byte == '\0')
            continue;
        if (byte == '\r') {
            if (i + 1 < text_len && text[i + 1] == '\n')
                continue;
            byte = '\n';
        }
        text[write_offset++] = byte;
    }
    text[write_offset] = '\0';
    return write_offset;
}

/* The read tool verifies content; the extension only hints at the already-sniffed data's type. */
static const char *extension_for_mime_type(const char *mime_type)
{
    if (strcmp(mime_type, "image/png") == 0)
        return ".png";
    if (strcmp(mime_type, "image/jpeg") == 0)
        return ".jpg";
    if (strcmp(mime_type, "image/gif") == 0)
        return ".gif";
    if (strcmp(mime_type, "image/webp") == 0)
        return ".webp";
    return "";
}

static char *persist_clipboard_image(const char *image, size_t image_len, const char *mime_type)
{
    char *path = NULL;
    int fd = tempfile_create("paste-", extension_for_mime_type(mime_type), &path);
    if (fd < 0)
        return NULL;
    int write_status = write_all(fd, image, image_len);
    close(fd);
    if (write_status < 0) {
        unlink(path);
        tempfile_untrack(path);
        free(path);
        return NULL;
    }
    /* Trailing space so the user can keep typing after the marker. */
    char *marker = xasprintf("[pasted image: %s] ", path);
    free(path);
    return marker;
}

static int hex_digit_value(char digit)
{
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

/* Accept only local file URIs. Keep malformed escapes verbatim, but reject decoded NULs because
 * downstream filesystem APIs would silently truncate the path. */
static char *file_uri_to_path(const char *uri, size_t uri_len)
{
    static const char SCHEME[] = "file://";
    if (uri_len < sizeof(SCHEME) - 1 || strncmp(uri, SCHEME, sizeof(SCHEME) - 1) != 0)
        return NULL;
    const char *cursor = uri + sizeof(SCHEME) - 1;
    const char *end = uri + uri_len;
    if (cursor < end && *cursor != '/') {
        const char *slash = memchr(cursor, '/', (size_t)(end - cursor));
        if (!slash || (size_t)(slash - cursor) != 9 || strncmp(cursor, "localhost", 9) != 0)
            return NULL;
        cursor = slash;
    }
    if (cursor >= end || *cursor != '/')
        return NULL;

    struct buf path;
    buf_init(&path);
    while (cursor < end) {
        char byte = *cursor;
        int high, low;
        if (byte == '%' && cursor + 2 < end && (high = hex_digit_value(cursor[1])) >= 0 &&
            (low = hex_digit_value(cursor[2])) >= 0) {
            byte = (char)((high << 4) | low);
            if (byte == '\0') {
                buf_free(&path);
                return NULL;
            }
            cursor += 3;
        } else {
            cursor++;
        }
        buf_append(&path, &byte, 1);
    }
    return buf_steal(&path);
}

/* Avoid filesystem access in the raw-mode editor: a URI may name a FIFO or stalled mount. */
static int path_has_image_extension(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (!extension)
        return 0;
    static const char *const IMAGE_EXTENSIONS[] = {".png", ".jpg", ".jpeg", ".gif", ".webp"};
    for (size_t i = 0; i < sizeof(IMAGE_EXTENSIONS) / sizeof(IMAGE_EXTENSIONS[0]); i++)
        if (strcasecmp(extension, IMAGE_EXTENSIONS[i]) == 0)
            return 1;
    return 0;
}

char *paste_image_uris_to_paths(const char *text)
{
    struct buf output;
    buf_init(&output);
    size_t converted_lines = 0;
    const char *line = text;
    while (*line) {
        const char *newline = strchr(line, '\n');
        size_t line_len = newline ? (size_t)(newline - line) : strlen(line);
        if (line_len > 0) {
            char *path = file_uri_to_path(line, line_len);
            if (!path) {
                buf_free(&output);
                return NULL;
            }
            if (converted_lines)
                buf_append(&output, "\n", 1);
            if (path_has_image_extension(path)) {
                char *marker = xasprintf("[pasted image: %s]", path);
                buf_append_str(&output, marker);
                free(marker);
            } else {
                buf_append_str(&output, path);
            }
            free(path);
            converted_lines++;
        }
        if (!newline)
            break;
        line = newline + 1;
    }
    if (!converted_lines) {
        buf_free(&output);
        return NULL;
    }
    buf_append(&output, " ", 1);
    return buf_steal(&output);
}

char *paste_image_capture(void)
{
    long deadline_ms = monotonic_ms() + CLIPBOARD_PASTE_TIMEOUT_MS;
    size_t image_len;
    char *image = clipboard_paste_image(&image_len, deadline_ms);
    if (image) {
        struct image_info info;
        char *marker = NULL;
        if (image_sniff(image, image_len, &info) && info.complete)
            marker = persist_clipboard_image(image, image_len, info.mime);
        free(image);
        if (marker)
            return marker;
    }

    size_t text_len;
    char *text = clipboard_paste_text(&text_len, deadline_ms);
    if (!text)
        return NULL;
    if (paste_image_normalize_text(text, text_len) == 0) {
        free(text);
        return NULL;
    }

    char *converted_uris = paste_image_uris_to_paths(text);
    if (converted_uris) {
        free(text);
        return converted_uris;
    }
    return text;
}
