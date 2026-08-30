/* SPDX-License-Identifier: MIT */
#include "tools/bash_output.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
/* The wait macros are provided by <sys/wait.h> per POSIX; glibc also leaks
 * them through <stdlib.h>, so the include cleaner cannot attribute them. */
#include <sys/wait.h> // IWYU pragma: keep

#include "buf.h"
#include "util.h"
#include "system/fs.h"
#include "system/tempfiles.h"
#include "text/utf8_sanitize.h"
#include "tools/output_cap.h"

/* Spill must precede the hard drain limit so killed producers are reported as truncated. */
#define BASH_OUTPUT_MEMORY_CAP_MAX (BASH_OUTPUT_DRAIN_LIMIT - 64L * 1024)

static void format_timeout_for_model(char *buf, size_t buf_len, long timeout_ms)
{
    if (timeout_ms % 1000 == 0)
        snprintf(buf, buf_len, "%lds", timeout_ms / 1000);
    else
        snprintf(buf, buf_len, "%ldms", timeout_ms);
}

struct bash_output {
    struct buf memory;
    size_t memory_cap;
    int spilled;
    int fd;     /* -1 until output spills */
    char *path; /* owned; NULL until output spills */
    int write_failed;
    size_t total_bytes;
    size_t newline_count;
    int has_partial_line;
};

static void output_init(struct bash_output *output, size_t memory_cap)
{
    buf_init(&output->memory);
    output->memory_cap =
        memory_cap > BASH_OUTPUT_MEMORY_CAP_MAX ? BASH_OUTPUT_MEMORY_CAP_MAX : memory_cap;
    output->spilled = 0;
    output->fd = -1;
    output->path = NULL;
    output->write_failed = 0;
    output->total_bytes = 0;
    output->newline_count = 0;
    output->has_partial_line = 0;
}

static size_t count_newlines(const char *data, size_t len)
{
    size_t count = 0;
    const char *cursor = data;
    size_t remaining = len;
    while (remaining > 0) {
        const char *newline = memchr(cursor, '\n', remaining);
        if (!newline)
            break;
        count++;
        size_t consumed = (size_t)(newline - cursor) + 1;
        cursor = newline + 1;
        remaining -= consumed;
    }
    return count;
}

/* The spill may contain binary bytes, so its extension must not imply a text encoding. */
static int output_open_tempfile(struct bash_output *output)
{
    output->fd = tempfile_create("bash-", ".log", &output->path);
    if (output->fd < 0) {
        output->write_failed = 1;
        return -1;
    }
    return 0;
}

static void output_spill(struct bash_output *output)
{
    /* Mark the attempt before opening so a failure is not retried for every later chunk. */
    output->spilled = 1;
    if (output_open_tempfile(output) < 0)
        return;
    if (output->memory.len > 0 &&
        write_all(output->fd, output->memory.data, output->memory.len) < 0)
        output->write_failed = 1;
    buf_free(&output->memory);
}

/* Totals describe produced output even when the backing file can no longer be written. */
static void output_write(struct bash_output *output, const char *data, size_t len)
{
    if (len == 0)
        return;
    size_t newlines = count_newlines(data, len);
    output->total_bytes += len;
    output->newline_count += newlines;
    output->has_partial_line = (data[len - 1] != '\n');
    if (output->write_failed && output->spilled)
        return;
    if (!output->spilled) {
        size_t line_count = output->newline_count + (output->has_partial_line ? 1 : 0);
        if (output->memory.len + len <= output->memory_cap && line_count <= OUTPUT_CAP_LINES) {
            buf_append(&output->memory, data, len);
            return;
        }
        output_spill(output);
        if (output->write_failed)
            return;
    }
    if (write_all(output->fd, data, len) < 0)
        output->write_failed = 1;
}

static void output_free(struct bash_output *output)
{
    buf_free(&output->memory);
    if (output->fd >= 0) {
        close(output->fd);
        output->fd = -1;
    }
    free(output->path);
    output->path = NULL;
}

static void output_unlink(struct bash_output *output)
{
    if (output->path) {
        unlink(output->path);
        tempfile_untrack(output->path);
        free(output->path);
        output->path = NULL;
    }
    if (output->fd >= 0) {
        close(output->fd);
        output->fd = -1;
    }
}

void bash_format_byte_size(char *buf, size_t buf_size, size_t bytes)
{
    if (bytes < 1024)
        snprintf(buf, buf_size, "%zuB", bytes);
    else if (bytes < 10L * 1024)
        snprintf(buf, buf_size, "%.1fK", (double)bytes / 1024.0);
    else if (bytes < 1024L * 1024)
        snprintf(buf, buf_size, "%zuK", (bytes + 512) / 1024);
    else if (bytes < 10L * 1024 * 1024)
        snprintf(buf, buf_size, "%.1fM", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, buf_size, "%zuM", (bytes + 512L * 1024) / (1024L * 1024));
}

/* Line capping may split a UTF-8 codepoint, so sanitize before building JSON output. */
void bash_output_append_sanitized(struct buf *body, const char *data, size_t len)
{
    size_t capped_len = 0;
    char *capped = cap_line_lengths(data ? data : "", len, OUTPUT_CAP_LINE_WIDTH, &capped_len);
    char *sanitized = utf8_sanitize(capped, capped_len);
    free(capped);
    buf_append_str(body, sanitized);
    free(sanitized);
}

int bash_read_head_slice(int fd, off_t range_start, size_t cap_bytes, size_t cap_lines,
                         off_t limit_off, struct buf *out, size_t *kept_bytes_out,
                         size_t *kept_lines_out)
{
    *kept_bytes_out = 0;
    *kept_lines_out = 0;
    size_t bytes_to_read = cap_bytes;
    if (limit_off >= 0) {
        size_t available = limit_off > range_start ? (size_t)(limit_off - range_start) : 0;
        if (bytes_to_read > available)
            bytes_to_read = available;
    }
    if (bytes_to_read == 0)
        return 0;

    char chunk[8192];
    while (out->len < bytes_to_read) {
        size_t remaining = bytes_to_read - out->len;
        ssize_t bytes_read = pread(fd, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk),
                                   range_start + (off_t)out->len);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (bytes_read == 0)
            break;
        buf_append(out, chunk, (size_t)bytes_read);
    }

    size_t last_newline = out->len;
    while (last_newline > 0 && out->data[last_newline - 1] != '\n')
        last_newline--;
    if (last_newline == 0) {
        out->len = 0;
        if (out->data)
            out->data[0] = '\0';
        return 0;
    }
    out->len = last_newline;

    size_t lines = count_newlines(out->data, out->len);
    if (lines > cap_lines) {
        size_t lines_seen = 0, offset = 0;
        while (offset < out->len && lines_seen < cap_lines) {
            if (out->data[offset] == '\n')
                lines_seen++;
            offset++;
        }
        out->len = offset;
        lines = cap_lines;
    }
    if (out->data)
        out->data[out->len] = '\0';

    *kept_bytes_out = out->len;
    *kept_lines_out = lines;
    return 0;
}

int bash_read_tail_slice(int fd, off_t range_start, size_t range_bytes, size_t cap_bytes,
                         size_t cap_lines, struct buf *out, size_t *kept_bytes_out,
                         size_t *kept_lines_out)
{
    size_t bytes_to_read = range_bytes < cap_bytes ? range_bytes : cap_bytes;
    off_t start = range_start + (off_t)(range_bytes - bytes_to_read);

    int needs_alignment = 0;
    if (start > range_start) {
        char previous_byte;
        ssize_t read_result;
        do {
            read_result = pread(fd, &previous_byte, 1, start - 1);
        } while (read_result < 0 && errno == EINTR);
        /* On read failure, fall back to the conservative behavior of
         * trimming through the first '\n'. We'd rather lose one line
         * than start mid-line. */
        needs_alignment = (read_result != 1) || (previous_byte != '\n');
    }

    char chunk[8192];
    while (out->len < bytes_to_read) {
        size_t remaining = bytes_to_read - out->len;
        ssize_t bytes_read = pread(fd, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk),
                                   start + (off_t)out->len);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (bytes_read == 0)
            break;
        buf_append(out, chunk, (size_t)bytes_read);
    }

    /* Do not trim when the only newline ends the window; that would erase a long line. */
    if (needs_alignment && out->len > 0) {
        char *newline = memchr(out->data, '\n', out->len);
        if (newline) {
            size_t skip_bytes = (size_t)(newline - out->data) + 1;
            if (skip_bytes < out->len) {
                memmove(out->data, out->data + skip_bytes, out->len - skip_bytes);
                out->len -= skip_bytes;
                if (out->data)
                    out->data[out->len] = '\0';
            }
        }
    }

    size_t newline_count = count_newlines(out->data ? out->data : "", out->len);
    size_t line_count = newline_count + (out->len > 0 && out->data[out->len - 1] != '\n' ? 1 : 0);
    if (line_count > cap_lines) {
        size_t lines_to_skip = line_count - cap_lines;
        size_t offset = 0;
        while (lines_to_skip > 0 && offset < out->len) {
            if (out->data[offset] == '\n')
                lines_to_skip--;
            offset++;
        }
        memmove(out->data, out->data + offset, out->len - offset);
        out->len -= offset;
        if (out->data)
            out->data[out->len] = '\0';
        line_count = cap_lines;
    }

    *kept_bytes_out = out->len;
    *kept_lines_out = line_count;
    return 0;
}

static void append_status(struct buf *out, enum bash_stop_reason reason, long timeout_ms,
                          int wait_status)
{
    if (reason == BASH_STOP_INTERRUPT) {
        buf_append_str(out, "\n[interrupted]");
        return;
    }
    if (reason == BASH_STOP_TIMEOUT) {
        char formatted_timeout[32];
        format_timeout_for_model(formatted_timeout, sizeof(formatted_timeout), timeout_ms);
        char footer[64];
        snprintf(footer, sizeof(footer), "\n[timed out after %s]", formatted_timeout);
        buf_append_str(out, footer);
        return;
    }
    if (WIFEXITED(wait_status)) {
        int code = WEXITSTATUS(wait_status);
        if (code != 0) {
            char footer[64];
            snprintf(footer, sizeof(footer), "\n[exit %d]", code);
            buf_append_str(out, footer);
        }
    } else if (WIFSIGNALED(wait_status)) {
        char footer[64];
        snprintf(footer, sizeof(footer), "\n[signal %d]", WTERMSIG(wait_status));
        buf_append_str(out, footer);
    }
    if (reason == BASH_STOP_ORPHANED) {
        char formatted_timeout[32];
        format_timeout_for_model(formatted_timeout, sizeof(formatted_timeout), timeout_ms);
        char footer[160];
        snprintf(footer, sizeof(footer),
                 "\n[orphaned processes killed after %s: a task tracks its shell, so drop '&' "
                 "or end the command with 'wait']",
                 formatted_timeout);
        buf_append_str(out, footer);
    }
}

static void append_run_suffix(struct buf *out, size_t total_bytes, int binary, int body_present,
                              enum bash_stop_reason reason, long timeout_ms, int wait_status)
{
    size_t before = out->len;
    if (binary) {
        char total_size[16];
        bash_format_byte_size(total_size, sizeof(total_size), total_bytes);
        char marker[64];
        snprintf(marker, sizeof(marker), "[binary output suppressed: %s]", total_size);
        buf_append_str(out, marker);
    }
    append_status(out, reason, timeout_ms, wait_status);
    if (out->len == before && !body_present)
        buf_append_str(out, "(no output)");
}

/* Takes ownership of `marker`; POSIX temp-file paths are not guaranteed to be UTF-8. */
static void append_sanitized_marker(struct buf *body, char *marker)
{
    char *clean = utf8_sanitize(marker, strlen(marker));
    free(marker);
    buf_append_str(body, clean);
    free(clean);
}

/* Embed model-facing truncation markers here; the live renderer applies its own elision. */
static void build_model_body(struct bash_output *output, int binary, struct buf *body)
{
    if (binary) {
        output_unlink(output);
        return;
    }

    if (!output->spilled) {
        bash_output_append_sanitized(body, output->memory.data, output->memory.len);
        output_unlink(output);
        return;
    }

    size_t line_count = output->newline_count + (output->has_partial_line ? 1 : 0);

    if (output->fd < 0 || output->write_failed) {
        /* Open failure retains the memory prefix; a partial file is not safe to advertise. */
        size_t kept_bytes = 0, kept_lines = 0;
        if (output->memory.len > 0) {
            bash_output_append_sanitized(body, output->memory.data, output->memory.len);
            kept_bytes = output->memory.len;
            kept_lines = count_newlines(output->memory.data, output->memory.len) +
                         (output->memory.data[output->memory.len - 1] != '\n' ? 1 : 0);
        }
        char kept_size[16], total_size[16];
        bash_format_byte_size(kept_size, sizeof(kept_size), kept_bytes);
        bash_format_byte_size(total_size, sizeof(total_size), output->total_bytes);
        append_sanitized_marker(body,
                                xasprintf("\n[output truncated: last %zu of %zu lines, %s of %s; "
                                          "full output unavailable (temp file write failed)]",
                                          kept_lines, line_count, kept_size, total_size));
        output_unlink(output);
        return;
    }

    size_t total_cap = output_cap_bytes();
    size_t head_cap_bytes = total_cap / BASH_OUTPUT_HEAD_DIVISOR;
    size_t head_cap_lines = OUTPUT_CAP_LINES / BASH_OUTPUT_HEAD_DIVISOR;
    size_t tail_cap_bytes = total_cap - head_cap_bytes;
    size_t tail_cap_lines = OUTPUT_CAP_LINES - head_cap_lines;

    struct buf tail_raw;
    buf_init(&tail_raw);
    size_t tail_bytes = 0, tail_lines = 0;
    if (bash_read_tail_slice(output->fd, 0, output->total_bytes, tail_cap_bytes, tail_cap_lines,
                             &tail_raw, &tail_bytes, &tail_lines) != 0) {
        buf_free(&tail_raw);
        append_sanitized_marker(body, xasprintf("\n[output truncated: %zu lines, full output "
                                                "unavailable (spill read failed)]",
                                                line_count));
        output_unlink(output);
        return;
    }

    off_t tail_offset = (off_t)(output->total_bytes - tail_bytes);
    struct buf head_raw;
    buf_init(&head_raw);
    size_t head_bytes = 0, head_lines = 0;
    bash_read_head_slice(output->fd, 0, head_cap_bytes, head_cap_lines, tail_offset, &head_raw,
                         &head_bytes, &head_lines);

    /* A byte gap can exist within one long line even when no whole lines were omitted. */
    size_t omitted_lines =
        line_count > head_lines + tail_lines ? line_count - head_lines - tail_lines : 0;
    size_t gap_bytes = (size_t)tail_offset - head_bytes;
    if (head_lines > 0 && gap_bytes > 0) {
        bash_output_append_sanitized(body, head_raw.data, head_raw.len);
        char *marker;
        if (omitted_lines > 0) {
            marker = xasprintf("... [output truncated: omitted %zu of %zu lines (kept first %zu, "
                               "last %zu); full output saved to %s] ...\n",
                               omitted_lines, line_count, head_lines, tail_lines, output->path);
        } else {
            char gap_size[16];
            bash_format_byte_size(gap_size, sizeof(gap_size), gap_bytes);
            marker = xasprintf("... [output truncated: omitted %s mid-line; full output saved to "
                               "%s] ...\n",
                               gap_size, output->path);
        }
        append_sanitized_marker(body, marker);
        bash_output_append_sanitized(body, tail_raw.data, tail_raw.len);
    } else {
        /* Tail-only fallback reclaims the budget initially reserved for the head. */
        buf_reset(&tail_raw);
        tail_bytes = 0;
        tail_lines = 0;
        if (bash_read_tail_slice(output->fd, 0, output->total_bytes, output_cap_bytes(),
                                 OUTPUT_CAP_LINES, &tail_raw, &tail_bytes, &tail_lines) != 0) {
            append_sanitized_marker(body, xasprintf("\n[output truncated: %zu lines, full output "
                                                    "unavailable (spill read failed)]",
                                                    line_count));
            output_unlink(output);
        } else {
            char kept_size[16], total_size[16];
            bash_format_byte_size(kept_size, sizeof(kept_size), tail_bytes);
            bash_format_byte_size(total_size, sizeof(total_size), output->total_bytes);
            bash_output_append_sanitized(body, tail_raw.data, tail_raw.len);
            append_sanitized_marker(
                body, xasprintf("\n[output truncated: last %zu of %zu lines, %s of %s; "
                                "full output saved to %s]",
                                tail_lines, line_count, kept_size, total_size, output->path));
        }
    }
    buf_free(&head_raw);
    buf_free(&tail_raw);
}

struct bash_output *bash_output_create(size_t memory_cap)
{
    struct bash_output *output = xmalloc(sizeof(*output));
    output_init(output, memory_cap);
    return output;
}

void bash_output_append(struct bash_output *output, const char *data, size_t len)
{
    output_write(output, data, len);
}

size_t bash_output_size(const struct bash_output *output)
{
    return output->total_bytes;
}

int bash_output_detach_file(struct bash_output *output, char **path_out)
{
    *path_out = NULL;
    if (!output->spilled)
        output_spill(output);
    if (output->fd < 0 || output->write_failed)
        return -1;
    int fd = output->fd;
    *path_out = output->path;
    output->fd = -1;
    output->path = NULL;
    return fd;
}

void bash_output_reattach_file(struct bash_output *output, int fd, char *path)
{
    output->fd = fd;
    output->path = path;
}

char *bash_output_format_suffix(size_t total_bytes, int binary, int body_present,
                                enum bash_stop_reason reason, long timeout_ms, int wait_status)
{
    struct buf suffix;
    buf_init(&suffix);
    append_run_suffix(&suffix, total_bytes, binary, body_present, reason, timeout_ms, wait_status);
    return buf_steal(&suffix);
}

char *bash_output_finish(struct bash_output *output, int binary, enum bash_stop_reason reason,
                         long timeout_ms, int wait_status)
{
    struct buf result;
    buf_init(&result);
    build_model_body(output, binary, &result);
    append_run_suffix(&result, output->total_bytes, binary, result.len > 0, reason, timeout_ms,
                      wait_status);
    return buf_steal(&result);
}

void bash_output_destroy(struct bash_output *output)
{
    if (!output)
        return;
    output_free(output);
    free(output);
}
