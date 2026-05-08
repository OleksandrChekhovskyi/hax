/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRACE_H
#define HAX_TRACE_H

#include <stddef.h>

/* Wire-level diagnostics. Activated by setting HAX_TRACE to a file path; the
 * file is opened lazily on first call and truncated, so each run starts fresh.
 * Output is line-buffered so `tail -f` works. When HAX_TRACE is unset, every
 * entry point below is a no-op. */

/* Force the lazy file-open so the truncate happens at startup, not on
 * first request. Without this, a run that never makes an HTTP call
 * (HAX_PROVIDER=mock, or an immediate Ctrl-D) would leave the previous
 * trace file intact, contradicting the truncate-on-startup guarantee.
 * No-op when HAX_TRACE is unset; safe to call multiple times. */
void trace_init(void);

int trace_enabled(void);

/* Emit a banner + headers (with Authorization redacted) + pretty-printed
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
