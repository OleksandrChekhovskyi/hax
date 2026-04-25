/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "provider.h"

static void oom(void)
{
    fprintf(stderr, "hax: out of memory\n");
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n)
        oom();
    return q;
}

char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    char *r = strdup(s);
    if (!r)
        oom();
    return r;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return NULL;
    }
    char *p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void gen_uuid_v4(char out[37])
{
    uint8_t b[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "hax: open /dev/urandom: %s\n", strerror(errno));
        abort();
    }
    size_t got = 0;
    while (got < sizeof(b)) {
        ssize_t r = read(fd, b + got, sizeof(b) - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "hax: read /dev/urandom: %s\n", strerror(errno));
            abort();
        }
        if (r == 0) {
            fprintf(stderr, "hax: unexpected EOF on /dev/urandom\n");
            abort();
        }
        got += (size_t)r;
    }
    close(fd);

    b[6] = (b[6] & 0x0f) | 0x40; /* RFC 4122 version 4 */
    b[8] = (b[8] & 0x3f) | 0x80; /* RFC 4122 variant */

    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],
             b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
}

char *slurp_file(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0)
        goto err_close;

    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf)
        goto err_close;

    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            goto err_free;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    buf[got] = '\0';
    close(fd);
    if (out_len)
        *out_len = got;
    return buf;

err_free:
    free(buf);
err_close:
    close(fd);
    return NULL;
}

char *slurp_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    char *buf = malloc(cap + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t got = 0;
    while (got < cap) {
        ssize_t r = read(fd, buf + got, cap - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }

    int truncated = 0;
    if (got == cap) {
        char probe;
        ssize_t r = read(fd, &probe, 1);
        if (r > 0)
            truncated = 1;
    }

    close(fd);
    buf[got] = '\0';
    if (out_len)
        *out_len = got;
    if (out_truncated)
        *out_truncated = truncated;
    return buf;
}

char *sanitize_utf8(const char *data, size_t len)
{
    struct buf out;
    buf_init(&out);
    const unsigned char *p = (const unsigned char *)data;
    const unsigned char *end = p + len;
    static const char replacement[] = "\xEF\xBF\xBD"; /* U+FFFD */

    while (p < end) {
        unsigned char c = *p;
        if (c == 0) {
            buf_append(&out, replacement, 3);
            p++;
            continue;
        }
        if (c < 0x80) {
            buf_append(&out, (const char *)&c, 1);
            p++;
            continue;
        }

        size_t n;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            n = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
            cp = c & 0x07;
        } else {
            buf_append(&out, replacement, 3);
            p++;
            continue;
        }

        if ((size_t)(end - p) < n) {
            buf_append(&out, replacement, 3);
            p++;
            continue;
        }

        int ok = 1;
        for (size_t i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                ok = 0;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }

        if (ok) {
            /* Strict UTF-8: reject overlong encodings, surrogates,
             * and code points beyond U+10FFFF. */
            if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
                (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
                ok = 0;
        }

        if (ok) {
            buf_append(&out, (const char *)p, n);
            p += n;
        } else {
            buf_append(&out, replacement, 3);
            p++;
        }
    }

    /* Guarantee a non-NULL return even when input was empty. */
    if (!out.data)
        return xstrdup("");
    return buf_steal(&out);
}

char *expand_home(const char *path)
{
    if (!path)
        return NULL;
    if (path[0] != '~')
        return xstrdup(path);
    const char *home = getenv("HOME");
    if (!home)
        return xstrdup(path);
    return xasprintf("%s%s", home, path + 1);
}

void buf_init(struct buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(struct buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void buf_grow(struct buf *b, size_t need)
{
    if (b->cap >= need)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(struct buf *b, const void *data, size_t n)
{
    buf_grow(b, b->len + n + 1);
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(struct buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_reset(struct buf *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

char *buf_steal(struct buf *b)
{
    char *p = b->data;
    buf_init(b);
    return p;
}

void item_free(struct item *it)
{
    if (!it)
        return;
    free(it->text);
    free(it->call_id);
    free(it->tool_name);
    free(it->tool_arguments_json);
    free(it->output);
    memset(it, 0, sizeof(*it));
}
