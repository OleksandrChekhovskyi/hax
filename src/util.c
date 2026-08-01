/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <assert.h>
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
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "config.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "text/utf8.h"

static int locale_is_utf8;

void locale_init_utf8(void)
{
    setlocale(LC_CTYPE, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") == 0) {
        locale_is_utf8 = 1;
        return;
    }
    if (setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "en_US.UTF-8"))
        locale_is_utf8 = 1;
}

int locale_have_utf8(void)
{
    return locale_is_utf8;
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

static void emit_diagnostic(const char *color, const char *format, va_list args)
{
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
    emit_diagnostic(theme_open(THEME_ERROR), format, args);
    va_end(args);
}

void hax_warn(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_diagnostic(theme_open(THEME_WARN), format, args);
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

int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 120;
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

int display_width(void)
{
    const char *mode = config_str("display_width");
    int configured_width = config_int("display_width");
    if (configured_width >= 20)
        return configured_width;

    int width = term_width();
    if ((!mode || strcasecmp(mode, "terminal") != 0) && width > DISPLAY_WIDTH_CAP)
        width = DISPLAY_WIDTH_CAP;
    return width < 20 ? 20 : width;
}

static size_t codepoint_cells_at(const char *str, size_t length, size_t offset,
                                 size_t *codepoint_bytes)
{
    int cells = utf8_codepoint_cells(str, length, offset, codepoint_bytes);
    return cells < 0 ? 1 : (size_t)cells;
}

/* Combining marks stay with the preceding visible codepoint across cuts. */
static size_t skip_zero_width(const char *str, size_t length, size_t offset)
{
    while (offset < length) {
        size_t codepoint_bytes;
        size_t cells = codepoint_cells_at(str, length, offset, &codepoint_bytes);
        if (cells != 0)
            break;
        offset += codepoint_bytes;
    }
    return offset;
}

static size_t advance_cells(const char *str, size_t length, size_t max_cells)
{
    size_t offset = 0;
    size_t cells = 0;
    while (offset < length && cells < max_cells) {
        size_t codepoint_bytes;
        size_t next_cells = codepoint_cells_at(str, length, offset, &codepoint_bytes);
        if (cells + next_cells > max_cells)
            break;
        cells += next_cells;
        offset += codepoint_bytes;
    }
    return skip_zero_width(str, length, offset);
}

size_t display_cells(const char *str)
{
    if (!str)
        return 0;

    size_t length = strlen(str);
    size_t offset = 0;
    size_t cells = 0;
    while (offset < length) {
        size_t codepoint_bytes;
        cells += codepoint_cells_at(str, length, offset, &codepoint_bytes);
        offset += codepoint_bytes;
    }
    return cells;
}

char *truncate_for_display(const char *str, size_t max_cells)
{
    if (!str)
        return xstrdup("");

    size_t length = strlen(str);
    if (length <= max_cells || advance_cells(str, length, max_cells) == length)
        return xstrdup(str);

    size_t content_cells = max_cells < 4 ? max_cells : max_cells - 3;
    size_t content_bytes = advance_cells(str, length, content_cells);
    size_t ellipsis_bytes = max_cells < 4 ? 0 : 3;
    char *result = xmalloc(content_bytes + ellipsis_bytes + 1);
    memcpy(result, str, content_bytes);
    memcpy(result + content_bytes, "...", ellipsis_bytes);
    result[content_bytes + ellipsis_bytes] = '\0';
    return result;
}

/* Unlike wrapping, ellipsis truncation may return zero rather than exceed max_cells. */
static size_t strict_break_pos(const char *str, size_t length, size_t max_cells,
                               size_t *next_offset)
{
    size_t offset = 0;
    size_t cells = 0;
    size_t last_space = SIZE_MAX;
    while (offset < length) {
        size_t codepoint_bytes;
        size_t next_cells = codepoint_cells_at(str, length, offset, &codepoint_bytes);
        if (cells + next_cells > max_cells) {
            /* A boundary space separates rows and does not consume a content cell. */
            if (str[offset] == ' ' && cells == max_cells)
                last_space = offset;
            break;
        }
        if (str[offset] == ' ')
            last_space = offset;
        cells += next_cells;
        offset += codepoint_bytes;
    }

    if (offset >= length) {
        if (next_offset)
            *next_offset = length;
        return length;
    }
    if (last_space == SIZE_MAX) {
        size_t row_end = advance_cells(str, length, max_cells);
        if (next_offset)
            *next_offset = row_end;
        return row_end;
    }

    size_t row_end = last_space;
    while (row_end > 0 && str[row_end - 1] == ' ')
        row_end--;
    if (next_offset)
        *next_offset = last_space + 1;
    return row_end;
}

size_t wrap_break_pos(const char *str, size_t length, size_t max_cells, size_t *next_offset)
{
    assert(max_cells >= 1);
    size_t next = 0;
    size_t row_end = strict_break_pos(str, length, max_cells, &next);
    if (row_end == 0 && next == 0 && length > 0) {
        /* Taking one oversized codepoint preserves forward progress. */
        row_end = skip_zero_width(str, length, utf8_next(str, length, 0));
        next = row_end;
    }
    if (next_offset)
        *next_offset = next;
    return row_end;
}

char *reflow_for_display(const char *str, int first_row_cells, int other_row_cells, int max_rows,
                         int last_row_reserve)
{
    if (!str)
        return xstrdup("");
    if (max_rows < 1)
        max_rows = 1;
    if (first_row_cells < 1)
        first_row_cells = 1;
    if (other_row_cells < 1)
        other_row_cells = 1;
    if (last_row_reserve < 0)
        last_row_reserve = 0;

    size_t length = strlen(str);
    int single_row_cells = first_row_cells - last_row_reserve;
    if (single_row_cells < 1)
        single_row_cells = 1;
    if (length <= (size_t)single_row_cells)
        return xstrdup(str);

    struct buf result;
    buf_init(&result);
    size_t offset = 0;
    for (int row = 0; row < max_rows; row++) {
        int row_cells = row == 0 ? first_row_cells : other_row_cells;
        /* Any emitted row may become the last, so all rows leave room for the suffix. */
        int content_cells = row_cells - last_row_reserve;
        if (content_cells < 1)
            content_cells = 1;

        size_t remaining = length - offset;
        if (advance_cells(str + offset, remaining, (size_t)content_cells) == remaining) {
            buf_append(&result, str + offset, remaining);
            break;
        }

        if (row == max_rows - 1) {
            int before_ellipsis_cells = content_cells - 3;
            if (before_ellipsis_cells < 1) {
                size_t row_bytes = advance_cells(str + offset, remaining, (size_t)content_cells);
                buf_append(&result, str + offset, row_bytes);
                break;
            }
            size_t row_bytes =
                strict_break_pos(str + offset, remaining, (size_t)before_ellipsis_cells, NULL);
            buf_append(&result, str + offset, row_bytes);
            buf_append(&result, "...", 3);
            break;
        }

        size_t next_offset;
        size_t row_bytes =
            wrap_break_pos(str + offset, remaining, (size_t)content_cells, &next_offset);
        buf_append(&result, str + offset, row_bytes);
        buf_append(&result, "\n", 1);
        offset += next_offset;
    }
    return buf_steal(&result);
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

char *cap_line_lengths(const char *data, size_t length, size_t max_line_bytes, size_t *out_len)
{
    struct buf result;
    buf_init(&result);
    size_t offset = 0;
    while (offset < length) {
        size_t line_start = offset;
        while (offset < length && data[offset] != '\n')
            offset++;
        size_t line_length = offset - line_start;
        if (line_length > max_line_bytes) {
            buf_append(&result, data + line_start, max_line_bytes);
            char marker[64];
            int marker_length = snprintf(marker, sizeof(marker), "...[%zu bytes elided]",
                                         line_length - max_line_bytes);
            buf_append(&result, marker, (size_t)marker_length);
        } else {
            buf_append(&result, data + line_start, line_length);
        }
        if (offset < length) {
            buf_append(&result, "\n", 1);
            offset++;
        }
    }
    if (out_len)
        *out_len = result.len;
    return buf_steal(&result);
}

/* Bounds invisible byte growth while preserving ordinary combining sequences. */
#define MAX_ZERO_WIDTH_PER_BASE 8

char *flatten_for_display(const char *str)
{
    if (!str)
        return xstrdup("");

    size_t length = strlen(str);
    /* Every transformation preserves, removes, or replaces input bytes with one byte. */
    char *result = xmalloc(length + 1);
    size_t result_length = 0;
    int previous_was_space = 1;
    int zero_width_run = 0;
    size_t offset = 0;
    while (offset < length) {
        unsigned char byte = (unsigned char)str[offset];
        if (byte < 0x80) {
            int is_space = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
                           byte < 0x20 || byte == 0x7f;
            if (is_space) {
                if (!previous_was_space) {
                    result[result_length++] = ' ';
                    previous_was_space = 1;
                }
            } else {
                result[result_length++] = (char)byte;
                previous_was_space = 0;
            }
            zero_width_run = 0;
            offset++;
            continue;
        }

        size_t codepoint_bytes;
        int cells = utf8_codepoint_cells(str, length, offset, &codepoint_bytes);
        if (cells < 0) {
            result[result_length++] = '?';
            zero_width_run = 0;
            previous_was_space = 0;
        } else if (cells == 0) {
            if (zero_width_run < MAX_ZERO_WIDTH_PER_BASE) {
                memcpy(result + result_length, str + offset, codepoint_bytes);
                result_length += codepoint_bytes;
                zero_width_run++;
            }
        } else {
            memcpy(result + result_length, str + offset, codepoint_bytes);
            result_length += codepoint_bytes;
            zero_width_run = 0;
            previous_was_space = 0;
        }
        offset += codepoint_bytes;
    }

    if (result_length > 0 && result[result_length - 1] == ' ')
        result_length--;
    result[result_length] = '\0';
    return result;
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

size_t output_cap_bytes(void)
{
    return (size_t)config_size("tool_output_cap");
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
    if (context_limit > 0) {
        char limit[32];
        long double ratio = (long double)context_tokens * 100.0L / (long double)context_limit;
        long percentage;
        if (ratio > LONG_MAX)
            percentage = LONG_MAX;
        else if (ratio < LONG_MIN)
            percentage = LONG_MIN;
        else
            percentage = (long)ratio;
        format_tokens(limit, sizeof(limit), context_limit);
        snprintf(out, out_size, "%s / %s (%ld%%)", used, limit, percentage);
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
