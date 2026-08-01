/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_OUTPUT_H
#define HAX_TOOLS_BASH_OUTPUT_H

#include <stddef.h>
#include <sys/types.h>

struct buf;

#define BASH_OUTPUT_DRAIN_LIMIT (16L * 1024 * 1024)

enum bash_stop_reason {
    BASH_STOP_NONE,
    BASH_STOP_TIMEOUT,
    BASH_STOP_INTERRUPT,
    BASH_STOP_ORPHANED, /* shell exited; orphans held the pipe past the deadline and were killed */
};

struct bash_output;

struct bash_output *bash_output_create(size_t memory_cap);
void bash_output_append(struct bash_output *output, const char *data, size_t len);
size_t bash_output_size(const struct bash_output *output);

/* Detach the accumulated bytes as an open tracked temp file positioned at EOF, forcing an early
 * spill when output still fits in memory. On success the caller owns the fd and *path_out and the
 * output must not be used except for bash_output_destroy. Returns -1 with *path_out NULL when the
 * spill file cannot be created or written. */
int bash_output_detach_file(struct bash_output *output, char **path_out);

/* Undo a successful detach, returning the fd and allocated path to the output so it can keep
 * accumulating and finish normally. */
void bash_output_reattach_file(struct bash_output *output, int fd, char *path);

/* Return an owned sanitized result, including truncation and status markers. */
char *bash_output_finish(struct bash_output *output, int binary, enum bash_stop_reason reason,
                         long timeout_ms, int wait_status);

/* Return an owned status suffix for output already sent to the live display. */
char *bash_output_format_suffix(size_t total_bytes, int binary, int body_present,
                                enum bash_stop_reason reason, long timeout_ms, int wait_status);

/* Compact human size: "12B", "1.2K", "40K", "1.5M". */
void bash_format_byte_size(char *buf, size_t buf_size, size_t bytes);

/* Shared output-shaping policy and helpers, used for both synchronous results and background
 * task deliveries so every truncated body follows one dialect. */

/* Fraction of the byte/line budget reserved for a leading summary; the rest goes to the
 * trailing results. */
#define BASH_OUTPUT_HEAD_DIVISOR 8

/* Append data with per-line width caps and UTF-8 sanitization applied. */
void bash_output_append_sanitized(struct buf *out, const char *data, size_t len);

/* Read a line-aligned head of the byte range starting at range_start, never reaching
 * limit_off. A long first line yields no head. Reads with pread; the fd offset is untouched.
 * Returns -1 on read failure. */
int bash_read_head_slice(int fd, off_t range_start, size_t cap_bytes, size_t cap_lines,
                         off_t limit_off, struct buf *out, size_t *kept_bytes_out,
                         size_t *kept_lines_out);

/* Read a line-aligned tail of the byte range [range_start, range_start + range_bytes),
 * retaining raw bytes when alignment would empty a long line. Reads with pread; the fd offset
 * is untouched. Returns -1 on read failure. */
int bash_read_tail_slice(int fd, off_t range_start, size_t range_bytes, size_t cap_bytes,
                         size_t cap_lines, struct buf *out, size_t *kept_bytes_out,
                         size_t *kept_lines_out);

void bash_output_destroy(struct bash_output *output);

#endif /* HAX_TOOLS_BASH_OUTPUT_H */
