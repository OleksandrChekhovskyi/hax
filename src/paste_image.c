/* SPDX-License-Identifier: MIT */
#include "paste_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "util.h"
#include "system/tempfiles.h"
#include "terminal/clipboard.h"
#include "tools/image_sniff.h"

size_t paste_image_normalize_text(char *s, size_t n)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\0')
            continue;
        if (c == '\r') {
            if (i + 1 < n && s[i + 1] == '\n')
                continue; /* CRLF: the LF that follows survives */
            c = '\n';     /* lone CR */
        }
        s[w++] = c;
    }
    s[w] = '\0';
    return w;
}

/* File extension for a sniffed mime type — cosmetic (read sniffs by
 * content), but a path ending in .png reads better in the prompt and
 * gives the model an honest hint. */
static const char *ext_for_mime(const char *mime)
{
    if (strcmp(mime, "image/png") == 0)
        return ".png";
    if (strcmp(mime, "image/jpeg") == 0)
        return ".jpg";
    if (strcmp(mime, "image/gif") == 0)
        return ".gif";
    if (strcmp(mime, "image/webp") == 0)
        return ".webp";
    return "";
}

/* Persist clipboard image bytes to a tracked temp file and format the
 * prompt marker. Returns NULL when the bytes couldn't be persisted. */
static char *persist_image(const char *bytes, size_t n, const char *mime)
{
    char *path = NULL;
    int fd = tempfile_create("paste-", ext_for_mime(mime), &path);
    if (fd < 0)
        return NULL;
    int wr = write_all(fd, bytes, n);
    close(fd);
    if (wr < 0) {
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

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decode one file:// URI into a malloc'd filesystem path, or NULL when
 * `s` isn't one. Accepts an empty or "localhost" authority; a real
 * remote host is not a local path. Malformed %-escapes are kept
 * verbatim rather than rejecting the whole paste, but a decoded %00 is
 * rejected: an embedded NUL would silently truncate the path under the
 * strlen-based consumers downstream. */
static char *uri_to_path(const char *s, size_t n)
{
    static const char SCHEME[] = "file://";
    if (n < sizeof(SCHEME) - 1 || strncmp(s, SCHEME, sizeof(SCHEME) - 1) != 0)
        return NULL;
    const char *p = s + sizeof(SCHEME) - 1;
    const char *end = s + n;
    if (p < end && *p != '/') {
        const char *slash = memchr(p, '/', (size_t)(end - p));
        if (!slash || (size_t)(slash - p) != 9 || strncmp(p, "localhost", 9) != 0)
            return NULL;
        p = slash;
    }
    if (p >= end || *p != '/')
        return NULL;

    struct buf out;
    buf_init(&out);
    while (p < end) {
        char c = *p;
        int hi, lo;
        if (c == '%' && p + 2 < end && (hi = hex_digit(p[1])) >= 0 && (lo = hex_digit(p[2])) >= 0) {
            c = (char)((hi << 4) | lo);
            if (c == '\0') {
                buf_free(&out);
                return NULL;
            }
            p += 3;
        } else {
            p++;
        }
        buf_append(&out, &c, 1);
    }
    return buf_steal(&out);
}

/* Extension-only image guess for the "[pasted image: …]" marker — a
 * cosmetic hint, so we deliberately don't open the file: a URI can name
 * a FIFO or a path on a stalled NFS/FUSE mount, and any stat/open would
 * block the raw-mode editor where Ctrl-C is only a queued byte. The read
 * tool sniffs by content and stays authoritative if the extension lies. */
static int file_is_image(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return 0;
    static const char *const EXTS[] = {".png", ".jpg", ".jpeg", ".gif", ".webp"};
    for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++)
        if (strcasecmp(dot, EXTS[i]) == 0)
            return 1;
    return 0;
}

char *paste_image_uris_to_paths(const char *text)
{
    struct buf out;
    buf_init(&out);
    int converted = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n > 0) {
            char *path = uri_to_path(p, n);
            if (!path) {
                buf_free(&out);
                return NULL; /* one non-URI line: not a file-manager paste */
            }
            if (converted)
                buf_append(&out, "\n", 1);
            if (file_is_image(path)) {
                char *marker = xasprintf("[pasted image: %s]", path);
                buf_append_str(&out, marker);
                free(marker);
            } else {
                buf_append_str(&out, path);
            }
            free(path);
            converted = 1;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!converted) {
        buf_free(&out);
        return NULL;
    }
    buf_append(&out, " ", 1);
    return buf_steal(&out);
}

char *paste_image_capture(void)
{
    /* One deadline for the whole operation — the image attempt and the
     * text fallback share it, so stalled helpers can't stack delays. */
    long deadline = monotonic_ms() + CLIPBOARD_PASTE_TIMEOUT_MS;
    size_t n = 0;
    char *bytes = clipboard_paste_image(&n, deadline);
    if (bytes) {
        /* Require a known signature and complete structure — the same
         * bar the read tool applies before shipping to a provider. */
        struct image_info info;
        char *marker = NULL;
        if (image_sniff(bytes, n, &info) && info.complete)
            marker = persist_image(bytes, n, info.mime);
        free(bytes);
        if (marker)
            return marker;
        /* No usable image — garbage bytes, or persistence failed on an
         * unusable $TMPDIR — so fall through to the clipboard's text
         * rather than making the paste a no-op. */
    }

    size_t tn = 0;
    char *text = clipboard_paste_text(&tn, deadline);
    if (!text)
        return NULL;
    if (paste_image_normalize_text(text, tn) == 0) {
        free(text);
        return NULL;
    }
    /* A file-manager "copy" or a drag-and-drop pastes file:// URIs;
     * convert to plain paths the model can read. */
    char *conv = paste_image_uris_to_paths(text);
    if (conv) {
        free(text);
        return conv;
    }
    return text;
}
