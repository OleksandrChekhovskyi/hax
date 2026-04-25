/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRACE_H
#define HAX_TRACE_H

#include <stddef.h>

/* Wire-level diagnostics. Activated by setting HAX_TRACE to a file path; the
 * file is opened lazily in append mode on first call and flushed line-by-line
 * so `tail -f` works. When HAX_TRACE is unset, every entry point below is a
 * no-op. */

int trace_enabled(void);

/* Emit a banner + headers (with Authorization redacted) + pretty-printed
 * request body. headers is a NULL-terminated array of "Key: Value" strings.
 * body need not be NUL-terminated; body_len is authoritative. */
void trace_request(const char *url, const char *const *headers, const char *body, size_t body_len);

/* Emit response status line, plus the error body if non-2xx. */
void trace_response_status(long status, const char *error_body);

/* Emit one parsed SSE event (event name + pretty-printed data payload). */
void trace_sse_event(const char *event_name, const char *data);

#endif /* HAX_TRACE_H */
