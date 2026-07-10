/* SPDX-License-Identifier: MIT */
#ifndef HAX_API_ERROR_H
#define HAX_API_ERROR_H

/* Format an HTTP error body into a concise human-readable message
 * suitable for the EV_ERROR surface. Two sources, in order of
 * preference:
 *
 *   1. Structured JSON. Walks the common shapes used by hosted
 *      providers and OpenAI-compatible servers:
 *        {"error": {"message": "..."}}    — OpenAI, Anthropic, most
 *        {"error": "..."}                 — older / simpler shapes
 *        {"message": "..."}               — Codex /responses, others
 *      Falls through to (2) when none of these resolve to a string.
 *
 *   2. Cleaned plain text. HTML tags stripped, whitespace runs
 *      collapsed, control bytes removed, truncated to a readable
 *      width (~200 chars). Covers the case where the proxy/gateway
 *      returned an HTML 5xx page (Cloudflare, nginx, Python's
 *      http.server, ...) — the user gets a one-liner instead of a
 *      multi-line wall of markup.
 *
 * Always prepends `HTTP <status>: ` when status > 0 so the underlying
 * cause is visible even when the body is empty or unparseable.
 *
 * Always returns a heap-owned, non-NULL string. Caller frees. */
char *format_api_error(long status, const char *body);

/* Diagnostic for a failed model-catalog fetch (GET <base_url>/models) in
 * the /model picker, keyed by the outcome http_get reported. Shared by
 * the openai and anthropic families, whose auth/endpoint shapes agree:
 *
 *   401/403      — API key rejected (has_key) or required but missing
 *   other 2xx    — server answered but http_get still failed: the body
 *                  was empty or the transfer died mid-body
 *   other status — plain HTTP failure, status shown
 *   status 0     — never reached; names base_url so "server down" vs
 *                  "wrong base_url" is self-diagnosing
 *
 * NULL `name` falls back to "provider". Heap-owned; caller frees. */
char *format_models_error(const char *name, const char *base_url, int has_key, long status);

#endif /* HAX_API_ERROR_H */
