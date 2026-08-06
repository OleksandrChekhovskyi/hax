/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSPORT_API_ERROR_H
#define HAX_TRANSPORT_API_ERROR_H

/* Extracts common JSON and SSE error shapes, or flattens a plain-text/HTML body. The result is
 * heap-owned, never NULL, and prefixed with the HTTP status when `status > 0`. */
char *format_api_error(long status, const char *body);

/* Formats a failed provider model-list request. `name == NULL` is rendered as "provider". The
 * result is heap-owned and never NULL. */
char *format_model_list_error(const char *name, const char *base_url, int has_key, long status);

#endif /* HAX_TRANSPORT_API_ERROR_H */
