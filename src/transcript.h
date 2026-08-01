/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSCRIPT_H
#define HAX_TRANSCRIPT_H

#include <stddef.h>
#include <stdio.h>

struct item;
struct tool_def;

/* ANSI styling is intended for a terminal or a pager configured to preserve SGR sequences. */
enum transcript_render_mode {
    TRANSCRIPT_RENDER_PLAIN,
    TRANSCRIPT_RENDER_ANSI,
};

/* Render the model-facing conversation to `out`. Text is unshortened; binary and opaque payloads
 * are represented by metadata. */
void transcript_render(FILE *out, const char *system_prompt, const struct tool_def *tools,
                       size_t n_tools, const struct item *items, size_t n_items);

/* Render the banner, system prompt, and advertised tools. */
void transcript_render_header(FILE *out, enum transcript_render_mode mode,
                              const char *system_prompt, const struct tool_def *tools,
                              size_t n_tools);

/* Render items[first_item, n_items). `turn_number` is incremented at each turn boundary.
 * Tool calls in the range must have their matching results in the same range. */
void transcript_render_items(FILE *out, enum transcript_render_mode mode, const struct item *items,
                             size_t n_items, size_t first_item, int *turn_number);

/* Truncate the configured transcript before session setup. No-op when logging is disabled. */
void transcript_log_init(void);

struct transcript_log;

/* Open the configured plain-text transcript and write its header. Returns NULL when logging is
 * disabled or the file cannot be opened. */
struct transcript_log *transcript_log_open(const char *system_prompt, const struct tool_def *tools,
                                           size_t n_tools);

/* Append items added since the previous call. NULL-safe. Call only after tool batches complete. */
void transcript_log_append(struct transcript_log *log, const struct item *items, size_t n_items);

/* Truncate the log and write a new header. NULL-safe. */
void transcript_log_reset(struct transcript_log *log, const char *system_prompt,
                          const struct tool_def *tools, size_t n_tools);

/* Flush, close, and free the log. NULL-safe. */
void transcript_log_close(struct transcript_log *log);

#endif /* HAX_TRANSCRIPT_H */
