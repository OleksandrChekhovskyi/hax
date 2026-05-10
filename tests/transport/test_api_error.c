/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "transport/api_error.h"

static void test_empty_body(void)
{
    char *m = format_api_error(500, NULL);
    EXPECT_STR_EQ(m, "HTTP 500");
    free(m);

    m = format_api_error(503, "");
    EXPECT_STR_EQ(m, "HTTP 503");
    free(m);
}

static void test_no_status_no_body(void)
{
    char *m = format_api_error(0, NULL);
    EXPECT_STR_EQ(m, "request failed");
    free(m);
}

static void test_openai_style_json(void)
{
    /* OpenAI-style nested error object — the most common shape. */
    char *m = format_api_error(429, "{\"error\":{\"message\":\"Rate limit exceeded\","
                                    "\"type\":\"rate_limit_error\"}}");
    EXPECT_STR_EQ(m, "HTTP 429: Rate limit exceeded");
    free(m);
}

static void test_simple_string_error(void)
{
    char *m = format_api_error(500, "{\"error\":\"upstream timeout\"}");
    EXPECT_STR_EQ(m, "HTTP 500: upstream timeout");
    free(m);
}

static void test_top_level_message(void)
{
    char *m = format_api_error(400, "{\"message\":\"Invalid model\"}");
    EXPECT_STR_EQ(m, "HTTP 400: Invalid model");
    free(m);
}

static void test_json_no_recognized_field(void)
{
    /* JSON parses but has none of the known keys → fall back to
     * cleaned plain text (the raw JSON) with status prefix. */
    char *m = format_api_error(500, "{\"foo\":\"bar\"}");
    /* Plain-text path: HTML stripping is no-op here, whitespace
     * collapse leaves it as-is. */
    EXPECT(strstr(m, "HTTP 500") != NULL);
    EXPECT(strstr(m, "foo") != NULL);
    free(m);
}

static void test_html_body_stripped(void)
{
    const char *html =
        "<!DOCTYPE HTML>\n<html lang=\"en\">\n  <head>\n    <title>Error</title>\n  </head>\n"
        "  <body>\n    <h1>Error response</h1>\n    <p>Error code: 500</p>\n"
        "    <p>Message: Internal Server Error.</p>\n  </body>\n</html>\n";
    char *m = format_api_error(500, html);
    /* No tags should survive. */
    EXPECT(strchr(m, '<') == NULL);
    EXPECT(strchr(m, '>') == NULL);
    /* The textual content should be visible somewhere. */
    EXPECT(strstr(m, "Error response") != NULL);
    EXPECT(strstr(m, "Internal Server Error") != NULL);
    /* Status prefix present. */
    EXPECT(strstr(m, "HTTP 500") != NULL);
    free(m);
}

static void test_long_message_truncated(void)
{
    /* Build a JSON payload with a 500-char message — should be truncated
     * with "..." at the end. */
    struct {
        char buf[1024];
    } x;
    char *p = x.buf;
    p += sprintf(p, "{\"error\":{\"message\":\"");
    for (int i = 0; i < 500; i++)
        *p++ = 'A';
    sprintf(p, "\"}}");
    char *m = format_api_error(500, x.buf);
    EXPECT(strstr(m, "...") != NULL);
    /* Total output should be much shorter than the input. */
    EXPECT(strlen(m) < 300);
    free(m);
}

static void test_transport_error_no_status(void)
{
    /* status=0 means libcurl error (DNS, connect refused, etc.) — the
     * provider passes the libcurl message as the body. No HTTP prefix
     * since there was no HTTP exchange. */
    char *m = format_api_error(0, "libcurl: Couldn't connect to server");
    EXPECT_STR_EQ(m, "libcurl: Couldn't connect to server");
    free(m);
}

static void test_html_with_style_and_script(void)
{
    /* Real Python http.server 5xx pages embed CSS in a <style> block;
     * many CDN/proxy pages also embed JS. Both should be stripped
     * along with the tags so only the human-readable text remains. */
    const char *html = "<html><head><style>:root { color: red; --x: 1px; }</style>"
                       "<script>alert('x');</script></head>"
                       "<body><h1>Service Unavailable</h1>"
                       "<p>Please try again.</p></body></html>";
    char *m = format_api_error(503, html);
    EXPECT(strstr(m, "Service Unavailable") != NULL);
    EXPECT(strstr(m, "Please try again") != NULL);
    /* CSS / JS bytes must NOT leak through. */
    EXPECT(strstr(m, "color") == NULL);
    EXPECT(strstr(m, "--x") == NULL);
    EXPECT(strstr(m, "alert") == NULL);
    free(m);
}

static void test_sse_framed_json(void)
{
    /* Some backends pack errors as SSE-shaped data even on non-2xx
     * responses. http.c suppresses SSE event delivery for non-2xx
     * but the captured err_body still has the framing — strip it
     * here so the structured message is extracted. */
    char *m = format_api_error(503, "data: {\"error\":{\"message\":\"upstream rate limit\"}}\n\n");
    EXPECT_STR_EQ(m, "HTTP 503: upstream rate limit");
    free(m);
}

static void test_sse_framed_top_level_message(void)
{
    char *m = format_api_error(500, "data: {\"message\":\"boom\"}\n\n");
    EXPECT_STR_EQ(m, "HTTP 500: boom");
    free(m);
}

static void test_sse_framed_with_crlf(void)
{
    /* CRLF line endings: trailing \r should be trimmed too. */
    char *m = format_api_error(503, "data: {\"error\":\"slow down\"}\r\n\r\n");
    EXPECT_STR_EQ(m, "HTTP 503: slow down");
    free(m);
}

static void test_sse_framed_malformed_json(void)
{
    /* Unwrap successful but payload isn't JSON → plain-text path
     * should display the unwrapped payload, NOT the original body
     * with the `data:` prefix still attached. */
    char *m = format_api_error(500, "data: not json here\n\n");
    EXPECT_STR_EQ(m, "HTTP 500: not json here");
    /* The `data:` prefix must not survive into the displayed message. */
    EXPECT(strstr(m, "data:") == NULL);
    free(m);
}

static void test_sse_framed_with_event_name(void)
{
    /* `event: error` precedes `data:` — the SSE parser pairs them up
     * automatically. Without using the parser, a naive prefix-strip
     * would fail to find `data:` at the start. */
    char *m = format_api_error(503, "event: error\n"
                                    "data: {\"error\":{\"message\":\"overloaded\"}}\n\n");
    EXPECT_STR_EQ(m, "HTTP 503: overloaded");
    free(m);
}

static void test_sse_framed_multiline_data(void)
{
    /* Per the SSE spec, multiple `data:` lines in one event are
     * concatenated with '\n'. The parser handles this; the joined
     * result here is valid JSON (newlines inside top-level whitespace
     * are fine for jansson). */
    char *m = format_api_error(500, "data: {\n"
                                    "data:   \"error\": \"slow down\"\n"
                                    "data: }\n\n");
    EXPECT_STR_EQ(m, "HTTP 500: slow down");
    free(m);
}

static void test_sse_framed_with_comments(void)
{
    /* `:` lines are SSE comments — keep-alive pings, etc. — and must
     * be ignored without disrupting the event extraction. */
    char *m = format_api_error(503, ": keep-alive\n"
                                    "event: error\n"
                                    ": another comment\n"
                                    "data: {\"error\":\"throttled\"}\n\n");
    EXPECT_STR_EQ(m, "HTTP 503: throttled");
    free(m);
}

/* Walk `s` and verify every byte sequence is a well-formed UTF-8
 * codepoint: 0xxxxxxx, 110xxxxx + 1×10xxxxxx, 1110xxxx + 2×10xxxxxx,
 * 11110xxx + 3×10xxxxxx. Returns 1 on success, 0 on any malformed
 * sequence. */
static int valid_utf8(const char *s)
{
    for (const char *q = s; *q;) {
        unsigned char c = (unsigned char)*q;
        int extra;
        if (c < 0x80)
            extra = 0;
        else if ((c & 0xE0) == 0xC0)
            extra = 1;
        else if ((c & 0xF0) == 0xE0)
            extra = 2;
        else if ((c & 0xF8) == 0xF0)
            extra = 3;
        else
            return 0;
        q++;
        for (int i = 0; i < extra; i++) {
            if (((unsigned char)*q & 0xC0) != 0x80)
                return 0;
            q++;
        }
    }
    return 1;
}

static void test_sse_skips_empty_event_then_extracts(void)
{
    /* A non-2xx body with a keep-alive `event: ping` (no data)
     * before the actual error event. The unwrap callback must
     * skip empty-data events and keep looking — returning 1 on
     * the first event regardless would lose the real message. */
    char *m = format_api_error(503, "event: ping\n\n"
                                    "event: error\n"
                                    "data: {\"error\":\"boom\"}\n\n");
    EXPECT_STR_EQ(m, "HTTP 503: boom");
    free(m);
}

static void test_sse_skips_data_ping_then_extracts(void)
{
    /* Some providers send data-bearing keep-alives before the error
     * event. Don't stop on the first non-empty data payload unless it
     * actually looks like an error. */
    char *m = format_api_error(503, "event: ping\n"
                                    "data: {\"type\":\"ping\"}\n\n"
                                    "event: error\n"
                                    "data: {\"error\":{\"message\":\"slow down\"}}\n\n");
    EXPECT_STR_EQ(m, "HTTP 503: slow down");
    free(m);
}

static void test_truncate_at_utf8_boundary(void)
{
    /* Build a JSON message where the truncation point lands inside
     * a 2-byte codepoint (é = 0xC3 0xA9). Naive byte truncation
     * would emit a lone 0xC3 + "..." → invalid UTF-8 in the
     * terminal. The cut should rewind to before the multi-byte
     * sequence and produce well-formed output. */
    char body[1024];
    char *p = body;
    p += sprintf(p, "{\"error\":\"");
    /* 199 ASCII bytes — pushes the cut into the next codepoint. */
    for (int i = 0; i < 199; i++)
        *p++ = 'A';
    *p++ = (char)0xC3; /* é leader */
    *p++ = (char)0xA9; /* é continuation */
    p += sprintf(p, " more text\"}");
    char *m = format_api_error(500, body);
    EXPECT(valid_utf8(m));
    /* The "..." marker should be present (body is longer than the cap). */
    EXPECT(strstr(m, "...") != NULL);
    free(m);
}

static void test_truncate_at_4byte_codepoint_boundary(void)
{
    /* Same idea as above but with a 4-byte codepoint (😀 = U+1F600,
     * UTF-8 0xF0 0x9F 0x98 0x80). The cut lands deep inside the
     * sequence (2 continuation bytes back from a leader); a single
     * utf8_prev call may not be enough — verify the loop
     * converges. */
    char body[1024];
    char *p = body;
    p += sprintf(p, "{\"error\":\"");
    for (int i = 0; i < 198; i++)
        *p++ = 'A';
    *p++ = (char)0xF0; /* 😀 leader */
    *p++ = (char)0x9F;
    *p++ = (char)0x98;
    *p++ = (char)0x80;
    p += sprintf(p, " trailing\"}");
    char *m = format_api_error(500, body);
    EXPECT(valid_utf8(m));
    free(m);
}

static void test_sse_only_event_no_data(void)
{
    /* `event: error` with no `data:` field — parser emits an event
     * with empty data. Our unwrap skips empty-data events and returns
     * NULL, so format_api_error falls through to plain-text on the
     * original body. */
    char *m = format_api_error(500, "event: error\n\n");
    /* The original body becomes the cleaned plain-text. */
    EXPECT(strstr(m, "HTTP 500") != NULL);
    EXPECT(strstr(m, "event") != NULL);
    free(m);
}

static void test_plain_text_with_angle_brackets(void)
{
    /* Plain-text error messages can legitimately contain `<` and `>`
     * as comparison operators or placeholder syntax — they must not
     * be treated as HTML tags and stripped. Without the looks-like-tag
     * gate, `<= 4096` would have everything from `<` onward swallowed
     * (no `>` ever closes the implied tag), losing the actual numeric
     * limit. */
    char *m = format_api_error(400, "max_tokens must be <= 4096");
    EXPECT_STR_EQ(m, "HTTP 400: max_tokens must be <= 4096");
    free(m);

    m = format_api_error(400, "value < 5 and value > 0");
    EXPECT_STR_EQ(m, "HTTP 400: value < 5 and value > 0");
    free(m);

    m = format_api_error(400, "expected <number>, got <string>");
    /* `<number>` and `<string>` start with letters, so they DO get
     * treated as tags and stripped. That's the intentional tradeoff —
     * angle-bracketed identifiers are ambiguous (could be HTML, could
     * be placeholder syntax) and we err on the HTML side. The
     * surrounding text survives. */
    EXPECT_STR_EQ(m, "HTTP 400: expected , got");
    free(m);
}

static void test_short_html_like_bodies(void)
{
    /* Bodies that look like the start of an HTML tag but are
     * truncated. The HTML stripper's tag_starts() helper must not
     * read past the NUL terminator. ASan would surface any
     * out-of-bounds read here. */
    const char *cases[] = {
        "<",  "<s",  "<sc",     "<scr", "<scri", "<scrip", "<script", "<style", "<style>partial",
        "</", "</s", "</style", "<!",   "<!--",  NULL,
    };
    for (size_t i = 0; cases[i]; i++) {
        char *m = format_api_error(500, cases[i]);
        EXPECT(m != NULL);
        EXPECT(strstr(m, "HTTP 500") != NULL);
        free(m);
    }
}

static void test_html_only_no_text(void)
{
    /* All-tags input → cleaned to empty → status-only output. */
    char *m = format_api_error(502, "<html><head></head><body></body></html>");
    EXPECT_STR_EQ(m, "HTTP 502");
    free(m);
}

static void test_multiline_collapsed(void)
{
    /* Newlines and tabs collapse to single spaces. */
    char *m = format_api_error(500, "line1\n\n\nline2\t\ttabbed");
    EXPECT_STR_EQ(m, "HTTP 500: line1 line2 tabbed");
    free(m);
}

int main(void)
{
    test_empty_body();
    test_no_status_no_body();
    test_openai_style_json();
    test_simple_string_error();
    test_top_level_message();
    test_json_no_recognized_field();
    test_html_body_stripped();
    test_html_with_style_and_script();
    test_sse_framed_json();
    test_sse_framed_top_level_message();
    test_sse_framed_with_crlf();
    test_sse_framed_malformed_json();
    test_sse_framed_with_event_name();
    test_sse_framed_multiline_data();
    test_sse_framed_with_comments();
    test_sse_skips_empty_event_then_extracts();
    test_sse_skips_data_ping_then_extracts();
    test_truncate_at_utf8_boundary();
    test_truncate_at_4byte_codepoint_boundary();
    test_sse_only_event_no_data();
    test_plain_text_with_angle_brackets();
    test_short_html_like_bodies();
    test_long_message_truncated();
    test_transport_error_no_status();
    test_html_only_no_text();
    test_multiline_collapsed();
    T_REPORT();
}
