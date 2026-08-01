/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_OUTPUT_H
#define HAX_TOOLS_BASH_OUTPUT_H

#include <stddef.h>

#define BASH_OUTPUT_DRAIN_LIMIT (16L * 1024 * 1024)

enum bash_stop_reason {
    BASH_STOP_NONE,
    BASH_STOP_TIMEOUT,
    BASH_STOP_INTERRUPT,
};

struct bash_output;

struct bash_output *bash_output_create(size_t memory_cap);
void bash_output_append(struct bash_output *output, const char *data, size_t len);
size_t bash_output_size(const struct bash_output *output);

/* Return an owned sanitized result, including truncation and status markers. */
char *bash_output_finish(struct bash_output *output, int binary, enum bash_stop_reason reason,
                         long timeout_ms, int wait_status);

/* Return an owned status suffix for output already sent to the live display. */
char *bash_output_format_suffix(size_t total_bytes, int binary, int body_present,
                                enum bash_stop_reason reason, long timeout_ms, int wait_status);

void bash_output_destroy(struct bash_output *output);

#endif /* HAX_TOOLS_BASH_OUTPUT_H */
