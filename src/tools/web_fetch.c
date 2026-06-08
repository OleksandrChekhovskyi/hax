/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <strings.h>

#include "text/html_markdown.h"
#include "text/utf8_sanitize.h"
#include "tool.h"
#include "transport/http.h"
#include "util.h"

/* Hard ceiling on how many bytes we pull off the wire, independent of the
 * (smaller) cap applied to what goes back to the model. Bounds memory for a
 * pathological multi-gigabyte response; 5 MB of HTML reduces to well under
 * the output cap after stripping. */
#define WEB_FETCH_MAX_DOWNLOAD (5 * 1024 * 1024)
#define WEB_FETCH_TIMEOUT_S    30L

/* Reject anything that isn't a plain http(s) URL, and URLs that smuggle
 * credentials in the authority (user:pass@host) — libcurl would happily send
 * them. Returns NULL when acceptable, or a malloc'd error message. */
static char *validate_url(const char *url)
{
    const char *rest;
    if (strncasecmp(url, "http://", 7) == 0)
        rest = url + 7;
    else if (strncasecmp(url, "https://", 8) == 0)
        rest = url + 8;
    else
        return xstrdup("url must be an http:// or https:// URL");

    /* The authority ends at the first '/', '?' or '#'. An '@' before that
     * point is userinfo. */
    for (const char *p = rest; *p && *p != '/' && *p != '?' && *p != '#'; p++) {
        if (*p == '@')
            return xstrdup("url must not contain embedded credentials (user:pass@host)");
    }
    return NULL;
}

/* Cap `s`/`len` at the shared output byte cap and UTF-8 sanitize it (web
 * bytes are arbitrary). Appends a truncation marker when either the download
 * or the output cap fired. Consumes nothing; returns a fresh string. */
static char *finalize_output(const char *s, size_t len, int download_truncated)
{
    size_t cap = output_cap_bytes();
    int output_truncated = 0;
    if (len > cap) {
        len = cap;
        output_truncated = 1;
    }

    char *clean = sanitize_utf8(s, len);

    if (output_truncated) {
        char *msg = xasprintf("%s\n\n[truncated at %zu bytes of output]", clean, cap);
        free(clean);
        return msg;
    }
    if (download_truncated) {
        char *msg = xasprintf("%s\n\n[truncated: response exceeded %d-byte download limit]", clean,
                              WEB_FETCH_MAX_DOWNLOAD);
        free(clean);
        return msg;
    }
    return clean;
}

static char *run(const char *args_json, tool_emit_display_fn emit_display, void *user)
{
    (void)emit_display;
    (void)user;

    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *url = json_string_value(json_object_get(root, "url"));
    if (!url || !*url) {
        json_decref(root);
        return xstrdup("missing 'url' argument");
    }
    json_t *jraw = json_object_get(root, "raw");
    int raw = jraw && json_is_true(jraw);

    char *bad = validate_url(url);
    if (bad) {
        json_decref(root);
        return bad;
    }

    char *url_dup = xstrdup(url);
    json_decref(root);

    struct http_fetch_result r;
    if (http_fetch(url_dup, WEB_FETCH_MAX_DOWNLOAD, WEB_FETCH_TIMEOUT_S, NULL, NULL, &r) < 0) {
        char *msg = xasprintf("error fetching %s: %s", url_dup, r.error ? r.error : "unknown");
        http_fetch_free(&r);
        free(url_dup);
        return msg;
    }

    /* Process the body identically for success and error responses: 4xx/5xx
     * pages are usually HTML with the real message in the prose (or JSON, for
     * an API), so converting them is far more useful than dumping raw markup. */
    int is_html = r.content_type && strstr(r.content_type, "html") != NULL;
    size_t blen = r.body_len;
    char *md = (is_html && !raw) ? html_to_markdown(r.body, r.body_len, &blen) : NULL;
    char *body = finalize_output(md ? md : r.body, md ? blen : r.body_len, r.truncated);
    free(md);

    char *out;
    if (r.status < 200 || r.status >= 300) {
        /* Prefix the status line; `body` is "" when the response had no body. */
        out = *body ? xasprintf("HTTP %ld fetching %s\n\n%s", r.status, url_dup, body)
                    : xasprintf("HTTP %ld fetching %s", r.status, url_dup);
        free(body);
    } else {
        out = body;
    }

    http_fetch_free(&r);
    free(url_dup);
    return out;
}

/* Surface a " (raw)" hint in the tool-call header when conversion is off, so
 * the user can tell at a glance that they're getting unprocessed bytes. */
static char *format_display_extra(const char *args_json)
{
    if (!args_json)
        return NULL;
    json_error_t jerr;
    json_t *root = json_loads(args_json, 0, &jerr);
    if (!root)
        return NULL;
    json_t *jraw = json_object_get(root, "raw");
    char *out = (jraw && json_is_true(jraw)) ? xstrdup(" (raw)") : NULL;
    json_decref(root);
    return out;
}

const struct tool TOOL_WEB_FETCH = {
    .def =
        {
            .name = "web_fetch",
            .description =
                "Fetch a URL over HTTP(S) and return its contents as text. Use it when you "
                "already have a specific URL to read — documentation, an article, a JSON API "
                "response. HTML is converted to readable Markdown; other content is returned "
                "unchanged. Only the initial HTML is fetched, so content a page renders with "
                "JavaScript won't appear (you'll get its shell) — fetch a raw or API URL for "
                "such sites. Only GET is supported; for other methods, request headers, or a "
                "request body, drive curl (or any client) through the `bash` tool.",
            .parameters_schema_json =
                "{\"type\":\"object\","
                "\"properties\":{"
                "\"url\":{\"type\":\"string\",\"description\":\"The http:// or https:// URL to "
                "fetch.\"},"
                "\"raw\":{\"type\":\"boolean\",\"description\":\"Return the raw response body, "
                "skipping HTML-to-Markdown conversion (no effect on non-HTML content). Default "
                "false.\"}"
                "},"
                "\"required\":[\"url\"]}",
            .display_arg = "url",
        },
    .run = run,
    .format_display_extra = format_display_extra,
    /* Like `read`: a fetched, converted page is output for the model, not the
     * user. Render just the one-line header (URL + spinner); the full content
     * still lands in history and the Ctrl-T / HAX_TRANSCRIPT views. */
    .silent_preview = 1,
};
