/* SPDX-License-Identifier: MIT */
#include "transport/api_error.h"

#include <ctype.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "text/utf8.h"
#include "transport/sse.h"

/* Hard cap on the displayed message length. Keeps a runaway HTML page
 * from blowing through the terminal even after tag-stripping; long
 * provider error JSON (rare, but possible) is also truncated to keep
 * the EV_ERROR line terminal-friendly. */
#define MAX_DISPLAY_LEN 200

/* Case-insensitive prefix-match against `p` (NUL-terminated input).
 * Returns 1 when `tag` matches AND is followed by a tag-name
 * delimiter (whitespace, '>', '/'). The NUL early-out means a
 * truncated body like "<scr" (strlen 4, looking for "script") is
 * rejected on the first NUL byte instead of relying on the
 * implicit `tolower(0) != 'i'` chain — same observable behaviour
 * but avoids reading past the buffer if a future caller ever
 * passes a non-NUL-terminated slice. */
static int tag_starts(const char *p, const char *tag)
{
    size_t n = strlen(tag);
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\0')
            return 0;
        if (tolower((unsigned char)p[i]) != tag[i])
            return 0;
    }
    /* Next byte must be a tag delimiter — otherwise "<scripture>"
     * would match "<script". A NUL here means the body ends
     * exactly at the tag name (e.g. "<style" with no further
     * bytes); treat that as a non-match too. */
    char nx = p[n];
    if (nx == '\0')
        return 0;
    return nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '>' || nx == '/';
}

/* Walk `body`, dropping bytes inside angle brackets (<...>) and
 * collapsing all whitespace runs (including newlines, tabs) to single
 * spaces. Also drops the *content* of <style> and <script> blocks —
 * naive tag-only stripping would otherwise leak CSS rules and JS
 * source into the visible error message. Strips control bytes
 * outside the angle-bracket regions too. Result is malloc'd and
 * trimmed of leading/trailing whitespace.
 *
 * Lightweight HTML stripper — doesn't handle entities (&amp; → &),
 * doesn't deal with CDATA, doesn't recover from unbalanced angle
 * brackets specially. The goal is "make a 500-byte HTML error page
 * readable on one line", not full HTML parsing. */
static char *strip_html_and_flatten(const char *body)
{
    struct buf out;
    buf_init(&out);
    int in_tag = 0;
    int last_was_space = 1;        /* suppress leading whitespace */
    const char *skip_until = NULL; /* when non-NULL, drop everything up to this lowercase tag */
    for (const char *p = body; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (skip_until) {
            /* Inside <style>/<script>: drop everything until the
             * matching closing tag. We still need to recognize the
             * "</tag" sequence, so a cheap scan suffices. */
            if (c == '<' && p[1] == '/' && tag_starts(p + 2, skip_until))
                skip_until = NULL;
            else
                continue;
        }
        if (in_tag) {
            if (c == '>')
                in_tag = 0;
            continue;
        }
        if (c == '<') {
            /* Only treat `<` as a tag opener when followed by something
             * that actually starts a tag name (ASCII letter, `/` for
             * close-tags, `!` for `<!DOCTYPE`/`<!--`, `?` for XML
             * processing instructions). A bare `<` followed by anything
             * else is plain text — comparison operators (`<= 4096`,
             * `value < 5`), placeholder syntax (`<value>` works on the
             * tag path; `< value` doesn't), or just a stray byte —
             * and dropping bytes until the next `>` would corrupt
             * non-HTML error messages. Eats the rest of the message
             * entirely when there's no closing `>` at all. */
            char next = p[1];
            int looks_like_tag = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                                 next == '/' || next == '!' || next == '?';
            if (looks_like_tag) {
                in_tag = 1;
                /* Detect opening <style> / <script> so we can also
                 * suppress their contents until the matching close. */
                if (tag_starts(p + 1, "style"))
                    skip_until = "style";
                else if (tag_starts(p + 1, "script"))
                    skip_until = "script";
                continue;
            }
            /* Fall through to emit the `<` as a literal byte. */
        }
        if (isspace(c) || c < 0x20) {
            if (!last_was_space) {
                buf_append(&out, " ", 1);
                last_was_space = 1;
            }
            continue;
        }
        buf_append(&out, p, 1);
        last_was_space = 0;
    }
    /* Trim trailing space from the run-collapse. */
    if (out.len > 0 && out.data[out.len - 1] == ' ') {
        out.data[out.len - 1] = '\0';
        out.len--;
    }
    /* buf_steal returns NULL on a never-appended buf. Normalize so
     * callers don't need a NULL check. */
    char *s = buf_steal(&out);
    return s ? s : xstrdup("");
}

/* Cap `s` at `max` bytes, walking back from the cut to the nearest
 * UTF-8 codepoint boundary so we never split a multi-byte sequence
 * in half. Without this, a long error message containing an
 * accented character at the cut position would emit a lone leading
 * byte (e.g. 0xC3 with no continuation) followed by "...", which
 * renders as garbage in the terminal. utf8_prev validates and
 * rewinds; on malformed UTF-8 it steps exactly one byte, so we
 * loop until we land on a position that's the start of a complete
 * preceding codepoint. The "..." marker is ASCII so the output
 * stays well-formed UTF-8. Caller frees. */
static char *truncate_chars(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max)
        return xstrdup(s);
    /* If the cut lands inside a multi-byte codepoint (the byte at
     * `cut` is a continuation 10xxxxxx), rewind via utf8_prev until
     * we land on a codepoint boundary. The loop matters for 3- and
     * 4-byte codepoints: cut can be 2+ continuation bytes deep, in
     * which case utf8_prev's single step may return i-1 (its
     * malformed-input fallback) without reaching the codepoint's
     * leader on the first try. utf8_prev always advances backward
     * by at least one byte, so the loop terminates. */
    size_t cut = max;
    while (cut > 0 && cut < n && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut = utf8_prev(s, cut);
    char *out = xmalloc(cut + 4);
    memcpy(out, s, cut);
    memcpy(out + cut, "...", 4);
    return out;
}

/* Pull a string from one of the recognized JSON error shapes. Returns
 * a borrowed pointer into `root`'s tree (valid until json_decref) or
 * NULL when no shape matches. */
static const char *extract_json_message(json_t *root)
{
    json_t *err = json_object_get(root, "error");
    if (json_is_object(err)) {
        json_t *m = json_object_get(err, "message");
        if (json_is_string(m))
            return json_string_value(m);
    } else if (json_is_string(err)) {
        return json_string_value(err);
    }
    json_t *m = json_object_get(root, "message");
    if (json_is_string(m))
        return json_string_value(m);
    return NULL;
}

struct unwrap_capture {
    char *fallback;
    char *best;
};

/* SSE-parser callback. Prefer an explicit `event: error` payload, or
 * any JSON data payload that has a recognized error/message shape.
 * Keep the first non-empty data as a fallback so plain unnamed SSE
 * error bodies still unwrap even when they aren't JSON. */
static int capture_error_event(const char *event, const char *data, void *user)
{
    struct unwrap_capture *cap = user;
    if (!data || !*data)
        return 0;

    int is_error = event && strcmp(event, "error") == 0;
    if (!is_error) {
        json_t *root = json_loads(data, 0, NULL);
        if (root) {
            is_error = extract_json_message(root) != NULL;
            json_decref(root);
        }
    }
    if (is_error) {
        cap->best = xstrdup(data);
        return 1;
    }
    if (!cap->fallback)
        cap->fallback = xstrdup(data);
    return 0;
}

/* If `body` is framed as Server-Sent Events, extract the error
 * event's data field so we can JSON-parse the structured error.
 * The http.c gate now suppresses SSE-event delivery on non-2xx
 * responses, but the captured err_body still has the raw framing —
 * without unwrapping, format_api_error would fall through to the
 * plain-text path and show `data: {"error":...}` to the user.
 *
 * Reuses the existing SSE parser so all framing variants land in
 * the same well-tested code path: data-bearing pings before the
 * real error, `event: error` + `data:` pairs,
 * multi-line `data:` continuations (joined with `\n` per the SSE
 * spec), `:` comment lines, CRLF endings. Bodies that aren't
 * SSE-shaped (plain JSON, HTML) emit no events — the parser sees
 * fields like `"error"` (no match against `event`/`data`/`id`/
 * `retry`) and silently ignores them — so unwrap returns NULL and
 * format_api_error falls through to direct JSON parsing of the
 * original body. Caller frees the returned payload. */
static char *unwrap_sse_data(const char *body)
{
    struct unwrap_capture cap = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_error_event, &cap);
    sse_parser_feed(&parser, body, strlen(body));
    sse_parser_finalize(&parser);
    sse_parser_free(&parser);
    if (cap.best) {
        free(cap.fallback);
        return cap.best;
    }
    return cap.fallback;
}

char *format_api_error(long status, const char *body)
{
    if (!body || !*body) {
        if (status > 0)
            return xasprintf("HTTP %ld", status);
        return xstrdup("request failed");
    }

    /* Unwrap SSE framing if present so an inline error event gets
     * the same JSON-extraction treatment as a plain JSON body. The
     * unwrapped string (when non-NULL) is preferred for both the
     * JSON path AND the plain-text fallback — otherwise a malformed
     * `data:` payload would still show the SSE prefix in the UI. */
    char *unwrapped = unwrap_sse_data(body);
    const char *content = unwrapped ? unwrapped : body;

    /* JSON path: prefer the structured message when we can extract one. */
    json_error_t jerr;
    json_t *root = json_loads(content, 0, &jerr);
    if (root) {
        const char *msg = extract_json_message(root);
        if (msg && *msg) {
            char *trimmed = truncate_chars(msg, MAX_DISPLAY_LEN);
            char *out =
                (status > 0) ? xasprintf("HTTP %ld: %s", status, trimmed) : xstrdup(trimmed);
            free(trimmed);
            json_decref(root);
            free(unwrapped);
            return out;
        }
        json_decref(root);
    }

    /* Plain-text / HTML path: clean it up before showing. */
    char *cleaned = strip_html_and_flatten(content);
    char *trimmed = truncate_chars(cleaned, MAX_DISPLAY_LEN);
    free(cleaned);
    char *out;
    if (status > 0) {
        if (*trimmed)
            out = xasprintf("HTTP %ld: %s", status, trimmed);
        else
            out = xasprintf("HTTP %ld", status);
    } else {
        out = xstrdup(*trimmed ? trimmed : "request failed");
    }
    free(trimmed);
    free(unwrapped);
    return out;
}
