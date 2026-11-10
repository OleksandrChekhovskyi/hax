/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSPORT_API_ERROR_H
#define HAX_TRANSPORT_API_ERROR_H

/* Extracts common JSON and SSE error shapes, or flattens a plain-text/HTML body. The result is
 * heap-owned, never NULL, and prefixed with the HTTP status when `status > 0`. */
char *format_api_error(long status, const char *body);

/* Formats a failed provider model-list request. `name == NULL` is rendered as "provider". The
 * result is heap-owned and never NULL. */
char *format_model_list_error(const char *name, const char *base_url, int has_key, long status);

/* What an HTTP error body says beyond its status: the server's native error code or type, and
 * its own error text — the field format_api_error displays, untruncated, or the whole body when
 * it is not JSON at all. A JSON body without a recognized field, or one that does not parse,
 * yields no text, since it may echo the request. Both members are owned and NULL when absent. */
struct api_error_body {
    char *code;
    char *detail;
};

/* `body` may be JSON, SSE-framed, or text, and may be NULL. */
struct api_error_body api_error_parse_body(const char *body);
void api_error_body_free(struct api_error_body *parsed);

/* Why a server rejected a request, as far as its error says. */
enum api_error_kind {
    API_ERROR_OTHER = 0,
    API_ERROR_CONTEXT_OVERFLOW,  /* the input exceeds the model's context window */
    API_ERROR_PAYLOAD_TOO_LARGE, /* the request body exceeds the endpoint's byte limit (413) */
};

/* Classifies a failure from its HTTP status (0 for one inside a stream) and what the server said
 * (either may be NULL). Only a client error other than a 429 can be an overflow, recognized by
 * known wording or by `overflow_phrases`, the provider's own NULL-terminated additions (may be
 * NULL), matched case-insensitively. */
enum api_error_kind api_error_classify(long status, const char *code, const char *detail,
                                       const char *const *overflow_phrases);

#endif /* HAX_TRANSPORT_API_ERROR_H */
