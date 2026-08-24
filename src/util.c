/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "terminal/ansi.h"
#include "terminal/theme.h"

static int locale_is_utf8;
static int locale_children_are_utf8;

void locale_init_utf8(void)
{
    locale_is_utf8 = 0;
    locale_children_are_utf8 = 0;

    setlocale(LC_CTYPE, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") == 0) {
        locale_is_utf8 = 1;
        locale_children_are_utf8 = 1;
        return;
    }
    /* OpenBSD ships no default locale at all, yet renders UTF-8 whatever the locale claims. This
     * process needs one regardless of the environment: mbrtowc() decodes the model's text here, and
     * without it every multibyte character is measured as its separate bytes.
     *
     * The spellings are PEP 538's, there being no portable one: glibc and the BSDs answer to
     * C.UTF-8, macOS only to a bare UTF-8. A language-bearing name is the last resort, for systems
     * where only specific locales were generated. */
    static const char *const CANDIDATES[] = {"C.UTF-8", "C.utf8", "UTF-8", "en_US.UTF-8"};
    const char *chosen = NULL;
    for (size_t i = 0; !chosen && i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++)
        chosen = setlocale(LC_CTYPE, CANDIDATES[i]);
    if (!chosen) {
        hax_warn("no UTF-8 locale found; non-ASCII text will be misread and misaligned");
        return;
    }
    locale_is_utf8 = 1;

    /* setlocale() reaches this process alone, so children need the choice published. A non-UTF-8
     * LC_ALL outranks LC_CTYPE and would mask it, and clearing it would move every other category
     * with it — leave the pinned locale alone and let callers render ASCII for children instead. */
    const char *lc_all = getenv("LC_ALL");
    if (lc_all && *lc_all)
        return;
    setenv("LC_CTYPE", chosen, 1);
    locale_children_are_utf8 = 1;
}

int locale_have_utf8(void)
{
    return locale_is_utf8;
}

const char *locale_child_ctype_override(void)
{
    if (locale_children_are_utf8 || !locale_is_utf8)
        return NULL;
    return setlocale(LC_CTYPE, NULL);
}

static void die_oom(void)
{
    fprintf(stderr, "hax: out of memory\n");
    abort();
}

/* Lets stdout presentation detect diagnostics emitted directly by lower layers. */
static _Atomic unsigned long diagnostic_sequence;

unsigned long hax_diag_sequence(void)
{
    return atomic_load_explicit(&diagnostic_sequence, memory_order_relaxed);
}

static hax_diag_fn diag_sink;
static void *diag_sink_user;

void hax_set_diag_sink(hax_diag_fn fn, void *user)
{
    diag_sink = fn;
    diag_sink_user = user;
}

static void emit_diagnostic(enum hax_diag_level level, const char *format, va_list args)
{
    if (diag_sink) {
        char *message = xvasprintf(format, args);
        if (message) {
            diag_sink(level, message, diag_sink_user);
            free(message);
        }
        atomic_fetch_add_explicit(&diagnostic_sequence, 1, memory_order_relaxed);
        return;
    }

    const char *color = theme_open(level == HAX_DIAG_WARN ? THEME_WARN : THEME_ERROR);
    int styled = isatty(fileno(stderr)) && *color;
    if (styled)
        fputs(color, stderr);
    fputs("hax: ", stderr);
    vfprintf(stderr, format, args);
    if (styled)
        fputs(ANSI_RESET, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    atomic_fetch_add_explicit(&diagnostic_sequence, 1, memory_order_relaxed);
}

void hax_err(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_diagnostic(HAX_DIAG_ERR, format, args);
    va_end(args);
}

void hax_warn(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_diagnostic(HAX_DIAG_WARN, format, args);
    va_end(args);
}

void *xmalloc(size_t size)
{
    void *result = malloc(size ? size : 1);
    if (!result)
        die_oom();
    return result;
}

void *xcalloc(size_t count, size_t element_size)
{
    void *result = (count && element_size) ? calloc(count, element_size) : calloc(1, 1);
    if (!result)
        die_oom();
    return result;
}

void *xrealloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size ? size : 1);
    if (!result)
        die_oom();
    return result;
}

char *xstrdup(const char *str)
{
    if (!str)
        return NULL;
    char *result = strdup(str);
    if (!result)
        die_oom();
    return result;
}

void string_array_free(char **strings)
{
    if (!strings)
        return;
    for (char **string = strings; *string; string++)
        free(*string);
    free(strings);
}

char **string_array_concat(const char *const *first, const char *const *second)
{
    size_t n_strings = 0;
    for (const char *const *string = first; string && *string; string++)
        n_strings++;
    for (const char *const *string = second; string && *string; string++)
        n_strings++;
    if (n_strings == 0)
        return NULL;

    char **combined = xmalloc(sizeof(*combined) * (n_strings + 1));
    size_t n = 0;
    for (const char *const *string = first; string && *string; string++)
        combined[n++] = xstrdup(*string);
    for (const char *const *string = second; string && *string; string++)
        combined[n++] = xstrdup(*string);
    combined[n] = NULL;
    return combined;
}

char *xvasprintf(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0)
        return NULL;

    char *result = xmalloc((size_t)length + 1);
    va_copy(copy, args);
    int written = vsnprintf(result, (size_t)length + 1, format, copy);
    va_end(copy);
    if (written < 0 || written > length) {
        free(result);
        return NULL;
    }
    return result;
}

char *xasprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *result = xvasprintf(format, args);
    va_end(args);
    return result;
}

char *shell_single_quote(const char *str)
{
    struct buf quoted;
    buf_init(&quoted);
    buf_append(&quoted, "'", 1);
    for (; str && *str; str++) {
        if (*str == '\'')
            buf_append_str(&quoted, "'\\''");
        else
            buf_append(&quoted, str, 1);
    }
    buf_append(&quoted, "'", 1);
    return buf_steal(&quoted);
}

void gen_uuid_v4(char out[37])
{
    uint8_t bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hax_err("open /dev/urandom: %s", strerror(errno));
        abort();
    }

    size_t bytes_read = 0;
    while (bytes_read < sizeof(bytes)) {
        ssize_t count = read(fd, bytes + bytes_read, sizeof(bytes) - bytes_read);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            hax_err("read /dev/urandom: %s", strerror(errno));
            abort();
        }
        if (count == 0) {
            hax_err("unexpected EOF on /dev/urandom");
            abort();
        }
        bytes_read += (size_t)count;
    }
    close(fd);

    bytes[6] = (bytes[6] & 0x0f) | 0x40; /* RFC 4122 version 4 */
    bytes[8] = (bytes[8] & 0x3f) | 0x80; /* RFC 4122 variant */

    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

int parse_int(const char *str, int *out)
{
    if (!str || !*str)
        return 0;

    char *end;
    errno = 0;
    long value = strtol(str, &end, 10);
    if (end == str || *end != '\0')
        return 0;
    if (errno == ERANGE || value > INT_MAX || value < INT_MIN)
        return 0;
    *out = (int)value;
    return 1;
}

int write_all(int fd, const void *data, size_t length)
{
    const char *cursor = data;
    while (length > 0) {
        size_t request = length > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : length;
        ssize_t written = write(fd, cursor, request);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        cursor += written;
        length -= (size_t)written;
    }
    return 0;
}

static int regular_mode_or_error(mode_t mode)
{
    if (S_ISREG(mode))
        return 0;
    errno = S_ISDIR(mode) ? EISDIR : EINVAL;
    return -1;
}

int ensure_regular_file(const char *path)
{
    struct stat status;
    if (stat(path, &status) < 0)
        return -1;
    return regular_mode_or_error(status.st_mode);
}

int open_regular_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -1;

    struct stat status;
    if (fstat(fd, &status) == 0 && regular_mode_or_error(status.st_mode) == 0)
        return fd;

    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

static ssize_t read_retry(int fd, void *data, size_t length)
{
    ssize_t bytes_read;
    do {
        bytes_read = read(fd, data, length);
    } while (bytes_read < 0 && errno == EINTR);
    return bytes_read;
}

char *slurp_file(const char *path, size_t *out_len)
{
    int saved_errno;
    int fd = open_regular_file(path);
    if (fd < 0)
        return NULL;

    struct buf contents;
    buf_init(&contents);
    char chunk[8192];
    for (;;) {
        ssize_t bytes_read = read_retry(fd, chunk, sizeof(chunk));
        if (bytes_read < 0)
            goto error;
        if (bytes_read == 0)
            break;
        buf_append(&contents, chunk, (size_t)bytes_read);
    }

    close(fd);
    if (out_len)
        *out_len = contents.len;
    return buf_steal(&contents);

error:
    saved_errno = errno;
    buf_free(&contents);
    close(fd);
    errno = saved_errno;
    return NULL;
}

char *slurp_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated)
{
    int saved_errno;
    int truncated = 0;
    int fd = open_regular_file(path);
    if (fd < 0)
        return NULL;

    struct buf contents;
    buf_init(&contents);
    char chunk[8192];
    while (contents.len < cap) {
        size_t remaining = cap - contents.len;
        size_t request = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        ssize_t bytes_read = read_retry(fd, chunk, request);
        if (bytes_read < 0)
            goto error;
        if (bytes_read == 0)
            break;
        buf_append(&contents, chunk, (size_t)bytes_read);
    }

    if (contents.len == cap) {
        char extra;
        ssize_t bytes_read = read_retry(fd, &extra, 1);
        if (bytes_read < 0)
            goto error;
        truncated = bytes_read > 0;
    }

    close(fd);
    if (out_len)
        *out_len = contents.len;
    if (out_truncated)
        *out_truncated = truncated;
    return buf_steal(&contents);

error:
    saved_errno = errno;
    buf_free(&contents);
    close(fd);
    errno = saved_errno;
    return NULL;
}

static char *xdg_hax_path(const char *env_name, const char *home_relative,
                          const char *relative_path)
{
    const char *xdg_base = getenv(env_name);
    if (xdg_base && *xdg_base)
        return xasprintf("%s/hax/%s", xdg_base, relative_path);
    const char *home = getenv("HOME");
    if (home && *home)
        return xasprintf("%s/%s/hax/%s", home, home_relative, relative_path);
    return NULL;
}

char *xdg_hax_config_path(const char *relative_path)
{
    return xdg_hax_path("XDG_CONFIG_HOME", ".config", relative_path);
}

char *xdg_hax_state_path(const char *relative_path)
{
    return xdg_hax_path("XDG_STATE_HOME", ".local/state", relative_path);
}

char *xdg_hax_cache_path(const char *relative_path)
{
    return xdg_hax_path("XDG_CACHE_HOME", ".cache", relative_path);
}

char *dup_trim_trailing_slash(const char *str)
{
    size_t length = strlen(str);
    while (length > 0 && str[length - 1] == '/')
        length--;
    char *result = xmalloc(length + 1);
    memcpy(result, str, length);
    result[length] = '\0';
    return result;
}

long parse_size(const char *str)
{
    if (!str || !*str)
        return 0;

    char *end;
    errno = 0;
    long value = strtol(str, &end, 10);
    if (end == str || value <= 0 || errno == ERANGE)
        return 0;
    while (*end == ' ' || *end == '\t')
        end++;

    long multiplier = 1;
    switch (*end) {
    case 'k':
    case 'K':
        multiplier = 1024L;
        end++;
        break;
    case 'm':
    case 'M':
        multiplier = 1024L * 1024L;
        end++;
        break;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0' || value > LONG_MAX / multiplier)
        return 0;
    return value * multiplier;
}

long parse_duration_ms(const char *str)
{
    if (!str || !*str)
        return -1;

    char *end;
    errno = 0;
    long value = strtol(str, &end, 10);
    if (end == str || value < 0 || errno == ERANGE)
        return -1;
    while (*end == ' ' || *end == '\t')
        end++;

    long multiplier;
    /* Match "ms" before "m". */
    if ((end[0] == 'm' || end[0] == 'M') && (end[1] == 's' || end[1] == 'S')) {
        multiplier = 1;
        end += 2;
    } else if (*end == '\0' || *end == 's' || *end == 'S') {
        multiplier = 1000;
        if (*end)
            end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 60000;
        end++;
    } else if (*end == 'h' || *end == 'H') {
        multiplier = 3600000;
        end++;
    } else {
        return -1;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0' || (multiplier > 1 && value > LONG_MAX / multiplier))
        return -1;
    return value * multiplier;
}

long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void format_tokens(char *out, size_t out_size, long tokens)
{
    if (tokens < 0)
        snprintf(out, out_size, "?");
    else if (tokens < 1024)
        snprintf(out, out_size, "%ld", tokens);
    else if (tokens < 10L * 1024)
        snprintf(out, out_size, "%.1fk", (double)tokens / 1024.0);
    else if (tokens < 1024L * 1024)
        snprintf(out, out_size, "%ldk", tokens / 1024 + (tokens % 1024 >= 512));
    else if (tokens < 10L * 1024 * 1024)
        snprintf(out, out_size, "%.1fM", (double)tokens / (1024.0 * 1024.0));
    else {
        const long million = 1024L * 1024L;
        snprintf(out, out_size, "%ldM", tokens / million + (tokens % million >= million / 2));
    }
}

void format_duration(char *out, size_t out_size, long duration_ms)
{
    long seconds = 0;
    if (duration_ms > 0)
        seconds = duration_ms / 1000 + (duration_ms % 1000 >= 500);

    if (seconds < 60)
        snprintf(out, out_size, "%lds", seconds);
    else if (seconds < 3600 && seconds % 60 == 0)
        snprintf(out, out_size, "%ldm", seconds / 60);
    else if (seconds < 3600)
        snprintf(out, out_size, "%ldm %02lds", seconds / 60, seconds % 60);
    else if (seconds % 3600 == 0)
        snprintf(out, out_size, "%ldh", seconds / 3600);
    else
        snprintf(out, out_size, "%ldh %02ldm", seconds / 3600, seconds % 3600 / 60);
}

void format_duration_steady(char *out, size_t out_size, long duration_ms)
{
    long seconds = 0;
    if (duration_ms > 0)
        seconds = duration_ms / 1000 + (duration_ms % 1000 >= 500);

    if (seconds < 60)
        snprintf(out, out_size, "%lds", seconds);
    else if (seconds < 3600)
        snprintf(out, out_size, "%ldm %02lds", seconds / 60, seconds % 60);
    else
        snprintf(out, out_size, "%ldh %02ldm", seconds / 3600, seconds % 3600 / 60);
}

void format_cost(char *out, size_t out_size, double usd)
{
    if (usd <= 0)
        snprintf(out, out_size, "$0.00");
    else if (usd < 0.01)
        snprintf(out, out_size, "$%.4f", usd);
    else if (usd < 1.0)
        snprintf(out, out_size, "$%.3f", usd);
    else
        snprintf(out, out_size, "$%.2f", usd);
}

void format_context(char *out, size_t out_size, long context_tokens, long context_limit)
{
    char used[32];
    format_tokens(used, sizeof(used), context_tokens);
    if (context_limit > 0 && context_tokens >= 0) {
        char limit[32];
        /* Usage above the window is real (stale model metadata), so report it rather than
         * capping at 100; the ceiling only keeps the field three digits wide. */
        double ratio = (double)context_tokens * 100.0 / (double)context_limit;
        long percentage = ratio > 999.0 ? 999 : (long)ratio;
        format_tokens(limit, sizeof(limit), context_limit);
        snprintf(out, out_size, "%s / %s (%ld%%)", used, limit, percentage);
    } else if (context_limit > 0) {
        char limit[32];
        format_tokens(limit, sizeof(limit), context_limit);
        snprintf(out, out_size, "%s / %s", used, limit);
    } else {
        snprintf(out, out_size, "%s", used);
    }
}

void buf_init(struct buf *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void buf_free(struct buf *buf)
{
    free(buf->data);
    buf_init(buf);
}

static void buf_grow(struct buf *buf, size_t required_capacity)
{
    if (buf->cap >= required_capacity)
        return;

    size_t capacity = buf->cap ? buf->cap : 256;
    while (capacity < required_capacity) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required_capacity;
            break;
        }
        capacity *= 2;
    }
    buf->data = xrealloc(buf->data, capacity);
    buf->cap = capacity;
}

void buf_append(struct buf *buf, const void *data, size_t length)
{
    if (buf->len == SIZE_MAX || length > SIZE_MAX - buf->len - 1)
        die_oom();
    buf_grow(buf, buf->len + length + 1);
    if (length > 0)
        memcpy(buf->data + buf->len, data, length);
    buf->len += length;
    buf->data[buf->len] = '\0';
}

void buf_append_str(struct buf *buf, const char *str)
{
    buf_append(buf, str, strlen(str));
}

void buf_reset(struct buf *buf)
{
    buf->len = 0;
    if (buf->data)
        buf->data[0] = '\0';
}

char *buf_steal(struct buf *buf)
{
    char *data = buf->data ? buf->data : xstrdup("");
    buf_init(buf);
    return data;
}
