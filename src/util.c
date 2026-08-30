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
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "diag.h"

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

void die_oom(void)
{
    fprintf(stderr, "hax: out of memory\n");
    abort();
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

void random_bytes(void *out, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hax_err("open /dev/urandom: %s", strerror(errno));
        abort();
    }

    size_t bytes_read = 0;
    while (bytes_read < len) {
        ssize_t count = read(fd, (char *)out + bytes_read, len - bytes_read);
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
}

void gen_uuid_v4(char out[37])
{
    uint8_t bytes[16];
    random_bytes(bytes, sizeof(bytes));

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

long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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
