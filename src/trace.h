/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRACE_H
#define HAX_TRACE_H

#include <stddef.h>

/* Wire-level diagnostics. trace_init resolves HAX_TRACE and truncates its file
 * before background HTTP jobs start. Output is line-buffered so `tail -f`
 * works. Until trace_init, and when HAX_TRACE is unset, every other entry point
 * is a no-op. */

/* Resolve and open the configured trace file. No-op when HAX_TRACE is unset;
 * safe to call multiple times. */
void trace_init(void);

int trace_enabled(void);

/* Emit a banner + headers (credential headers — Authorization, x-api-key,
 * api-key — redacted) + pretty-printed
 * request body. method is "POST", "GET", … and goes into the banner.
 * headers is a NULL-terminated array of "Key: Value" strings. body need
 * not be NUL-terminated; body_len is authoritative. body may be NULL/0
 * (e.g. for GETs). */
void trace_request(const char *method, const char *url, const char *const *headers,
                   const char *body, size_t body_len);

/* Emit response status line, plus the error body if non-2xx. */
void trace_response_status(long status, const char *error_body);

/* Emit one parsed SSE event (event name + pretty-printed data payload). */
void trace_sse_event(const char *event_name, const char *data);

#endif /* HAX_TRACE_H */
