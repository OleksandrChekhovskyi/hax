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

#endif /* HAX_API_ERROR_H */
