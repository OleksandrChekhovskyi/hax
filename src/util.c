/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "provider.h"

static int locale_utf8 = 0;

void locale_init_utf8(void)
{
    setlocale(LC_CTYPE, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") == 0) {
        locale_utf8 = 1;
        return;
    }
    if (setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "en_US.UTF-8"))
        locale_utf8 = 1;
}

int locale_have_utf8(void)
{
    return locale_utf8;
}

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

int term_width(void)
{
    struct winsize ws;
    int w = 120;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        w = ws.ws_col;
    if (w < 40)
        w = 40;
    if (w > 200)
        w = 200;
    return w;
}

int write_all(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int ensure_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode)) {
        errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        return -1;
    }
    return 0;
}

char *slurp_file(const char *path, size_t *out_len)
{
    if (ensure_regular_file(path) < 0)
        return NULL;

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
    if (ensure_regular_file(path) < 0)
        return NULL;

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

char *cap_line_lengths(const char *data, size_t len, size_t max_line, size_t *out_len)
{
    struct buf out;
    buf_init(&out);
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && data[i] != '\n')
            i++;
        size_t line_len = i - line_start;
        if (line_len > max_line) {
            buf_append(&out, data + line_start, max_line);
            char marker[64];
            int m = snprintf(marker, sizeof(marker), "...[%zu bytes elided]", line_len - max_line);
            buf_append(&out, marker, (size_t)m);
        } else {
            buf_append(&out, data + line_start, line_len);
        }
        if (i < len) {
            buf_append(&out, "\n", 1);
            i++;
        }
    }
    if (!out.data)
        out.data = xstrdup("");
    if (out_len)
        *out_len = out.len;
    return buf_steal(&out);
}

char *flatten_for_display(const char *s)
{
    if (!s)
        return xstrdup("");
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    size_t j = 0;
    int prev_space = 1; /* drop leading whitespace */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int is_space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c < 0x20 || c == 0x7f;
        if (is_space) {
            if (!prev_space) {
                out[j++] = ' ';
                prev_space = 1;
            }
        } else {
            out[j++] = (char)c;
            prev_space = 0;
        }
    }
    while (j > 0 && out[j - 1] == ' ')
        j--;
    out[j] = '\0';
    return out;
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

/* Shared resolver behind xdg_hax_config_path / xdg_hax_state_path:
 * "$<env_var>/hax/<relpath>" when the env var is set and non-empty,
 * else "$HOME/<home_default>/hax/<relpath>". Returns NULL when neither
 * is available. */
static char *xdg_hax_path(const char *env_var, const char *home_default, const char *relpath)
{
    const char *xdg = getenv(env_var);
    if (xdg && *xdg)
        return xasprintf("%s/hax/%s", xdg, relpath);
    const char *home = getenv("HOME");
    if (home && *home)
        return xasprintf("%s/%s/hax/%s", home, home_default, relpath);
    return NULL;
}

char *xdg_hax_config_path(const char *relpath)
{
    return xdg_hax_path("XDG_CONFIG_HOME", ".config", relpath);
}

char *xdg_hax_state_path(const char *relpath)
{
    return xdg_hax_path("XDG_STATE_HOME", ".local/state", relpath);
}

char *dup_trim_trailing_slash(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/')
        n--;
    char *out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

long parse_duration_ms(const char *s)
{
    if (!s || !*s)
        return -1;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    /* ERANGE catches out-of-range numerals like "99...99ms" — strtol
     * clamps to LONG_MAX, and the mul==1 ms path below would otherwise
     * skip the overflow guard and silently accept a near-LONG_MAX value. */
    if (end == s || v < 0 || errno == ERANGE)
        return -1;
    while (*end == ' ' || *end == '\t')
        end++;
    long mul;
    /* `ms` must be matched before bare `m` so "5ms" doesn't parse as
     * "5m" (300_000) followed by stray "s". */
    if ((end[0] == 'm' || end[0] == 'M') && (end[1] == 's' || end[1] == 'S')) {
        mul = 1;
        end += 2;
    } else if (*end == '\0' || *end == 's' || *end == 'S') {
        mul = 1000;
        if (*end)
            end++;
    } else if (*end == 'm' || *end == 'M') {
        mul = 60000;
        end++;
    } else if (*end == 'h' || *end == 'H') {
        mul = 3600000;
        end++;
    } else {
        return -1;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return -1;
    /* Reject values that would overflow when scaled. v is non-negative;
     * only the multiply needs guarding. */
    if (mul > 1 && v > LONG_MAX / mul)
        return -1;
    return v * mul;
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
    free(it->reasoning_json);
    memset(it, 0, sizeof(*it));
}
