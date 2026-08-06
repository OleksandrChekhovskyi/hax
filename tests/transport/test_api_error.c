/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "text/utf8.h"
#include "transport/api_error.h"

static void test_empty_body(void)
{
    char *message = format_api_error(500, NULL);
    EXPECT_STR_EQ(message, "HTTP 500");
    free(message);

    message = format_api_error(503, "");
    EXPECT_STR_EQ(message, "HTTP 503");
    free(message);
}

static void test_no_status_no_body(void)
{
    char *message = format_api_error(0, NULL);
    EXPECT_STR_EQ(message, "request failed");
    free(message);
}

static void test_openai_style_json(void)
{
    char *message = format_api_error(429, "{\"error\":{\"message\":\"Rate limit exceeded\","
                                          "\"type\":\"rate_limit_error\"}}");
    EXPECT_STR_EQ(message, "HTTP 429: Rate limit exceeded");
    free(message);
}

static void test_simple_string_error(void)
{
    char *message = format_api_error(500, "{\"error\":\"upstream timeout\"}");
    EXPECT_STR_EQ(message, "HTTP 500: upstream timeout");
    free(message);
}

static void test_top_level_message(void)
{
    char *message = format_api_error(400, "{\"message\":\"Invalid model\"}");
    EXPECT_STR_EQ(message, "HTTP 400: Invalid model");
    free(message);
}

static void test_json_no_recognized_field(void)
{
    char *message = format_api_error(500, "{\"foo\":\"bar\"}");
    EXPECT(strstr(message, "HTTP 500") != NULL);
    EXPECT(strstr(message, "foo") != NULL);
    free(message);
}

static void test_html_body_stripped(void)
{
    const char *html =
        "<!DOCTYPE HTML>\n<html lang=\"en\">\n  <head>\n    <title>Error</title>\n  </head>\n"
        "  <body>\n    <h1>Error response</h1>\n    <p>Error code: 500</p>\n"
        "    <p>Message: Internal Server Error.</p>\n  </body>\n</html>\n";
    char *message = format_api_error(500, html);
    EXPECT(strchr(message, '<') == NULL);
    EXPECT(strchr(message, '>') == NULL);
    EXPECT(strstr(message, "Error response") != NULL);
    EXPECT(strstr(message, "Internal Server Error") != NULL);
    EXPECT(strstr(message, "HTTP 500") != NULL);
    free(message);
}

static void test_long_message_truncated(void)
{
    char body[1024];
    char *cursor = body;
    cursor += sprintf(cursor, "{\"error\":{\"message\":\"");
    for (int i = 0; i < 500; i++)
        *cursor++ = 'A';
    sprintf(cursor, "\"}}");
    char *message = format_api_error(500, body);
    EXPECT(strstr(message, "...") != NULL);
    EXPECT(strlen(message) < 300);
    free(message);
}

static void test_transport_error_no_status(void)
{
    char *message = format_api_error(0, "libcurl: Couldn't connect to server");
    EXPECT_STR_EQ(message, "libcurl: Couldn't connect to server");
    free(message);
}

static void test_html_with_style_and_script(void)
{
    const char *html = "<html><head><style>:root { color: red; --x: 1px; }</style>"
                       "<script>alert('x');</script></head>"
                       "<body><h1>Service Unavailable</h1>"
                       "<p>Please try again.</p></body></html>";
    char *message = format_api_error(503, html);
    EXPECT(strstr(message, "Service Unavailable") != NULL);
    EXPECT(strstr(message, "Please try again") != NULL);
    EXPECT(strstr(message, "color") == NULL);
    EXPECT(strstr(message, "--x") == NULL);
    EXPECT(strstr(message, "alert") == NULL);
    free(message);
}

static void test_sse_framed_json(void)
{
    char *message =
        format_api_error(503, "data: {\"error\":{\"message\":\"upstream rate limit\"}}\n\n");
    EXPECT_STR_EQ(message, "HTTP 503: upstream rate limit");
    free(message);
}

static void test_sse_framed_top_level_message(void)
{
    char *message = format_api_error(500, "data: {\"message\":\"boom\"}\n\n");
    EXPECT_STR_EQ(message, "HTTP 500: boom");
    free(message);
}

static void test_sse_framed_with_crlf(void)
{
    char *message = format_api_error(503, "data: {\"error\":\"slow down\"}\r\n\r\n");
    EXPECT_STR_EQ(message, "HTTP 503: slow down");
    free(message);
}

static void test_sse_framed_malformed_json(void)
{
    char *message = format_api_error(500, "data: not json here\n\n");
    EXPECT_STR_EQ(message, "HTTP 500: not json here");
    EXPECT(strstr(message, "data:") == NULL);
    free(message);
}

static void test_sse_framed_with_event_name(void)
{
    char *message = format_api_error(503, "event: error\n"
                                          "data: {\"error\":{\"message\":\"overloaded\"}}\n\n");
    EXPECT_STR_EQ(message, "HTTP 503: overloaded");
    free(message);
}

static void test_sse_framed_multiline_data(void)
{
    char *message = format_api_error(500, "data: {\n"
                                          "data:   \"error\": \"slow down\"\n"
                                          "data: }\n\n");
    EXPECT_STR_EQ(message, "HTTP 500: slow down");
    free(message);
}

static void test_sse_framed_with_comments(void)
{
    char *message = format_api_error(503, ": keep-alive\n"
                                          "event: error\n"
                                          ": another comment\n"
                                          "data: {\"error\":\"throttled\"}\n\n");
    EXPECT_STR_EQ(message, "HTTP 503: throttled");
    free(message);
}

static int valid_utf8(const char *text)
{
    return utf8_is_valid(text, strlen(text));
}

static void test_sse_skips_empty_event_then_extracts(void)
{
    char *message = format_api_error(503, "event: ping\n\n"
                                          "event: error\n"
                                          "data: {\"error\":\"boom\"}\n\n");
    EXPECT_STR_EQ(message, "HTTP 503: boom");
    free(message);
}

static void test_sse_skips_data_ping_then_extracts(void)
{
    char *message = format_api_error(503, "event: ping\n"
                                          "data: {\"type\":\"ping\"}\n\n"
                                          "event: error\n"
                                          "data: {\"error\":{\"message\":\"slow down\"}}\n\n");
    EXPECT_STR_EQ(message, "HTTP 503: slow down");
    free(message);
}

static void test_truncate_at_utf8_boundary(void)
{
    /* Put the 200-byte cutoff inside é. */
    char body[1024];
    char *cursor = body;
    cursor += sprintf(cursor, "{\"error\":\"");
    for (int i = 0; i < 199; i++)
        *cursor++ = 'A';
    *cursor++ = (char)0xC3; /* é leader */
    *cursor++ = (char)0xA9; /* é continuation */
    sprintf(cursor, " more text\"}");
    char *message = format_api_error(500, body);
    EXPECT(valid_utf8(message));
    EXPECT(strstr(message, "...") != NULL);
    free(message);
}

static void test_truncate_at_4byte_codepoint_boundary(void)
{
    /* Put the 200-byte cutoff inside a four-byte codepoint. */
    char body[1024];
    char *cursor = body;
    cursor += sprintf(cursor, "{\"error\":\"");
    for (int i = 0; i < 198; i++)
        *cursor++ = 'A';
    *cursor++ = (char)0xF0; /* 😀 leader */
    *cursor++ = (char)0x9F;
    *cursor++ = (char)0x98;
    *cursor++ = (char)0x80;
    sprintf(cursor, " trailing\"}");
    char *message = format_api_error(500, body);
    EXPECT(valid_utf8(message));
    free(message);
}

static void test_sse_only_event_no_data(void)
{
    char *message = format_api_error(500, "event: error\n\n");
    EXPECT(strstr(message, "HTTP 500") != NULL);
    EXPECT(strstr(message, "event") != NULL);
    free(message);
}

static void test_plain_text_with_angle_brackets(void)
{
    char *message = format_api_error(400, "max_tokens must be <= 4096");
    EXPECT_STR_EQ(message, "HTTP 400: max_tokens must be <= 4096");
    free(message);

    message = format_api_error(400, "value < 5 and value > 0");
    EXPECT_STR_EQ(message, "HTTP 400: value < 5 and value > 0");
    free(message);

    message = format_api_error(400, "expected <number>, got <string>");
    /* Letter-prefixed angle brackets are intentionally treated as tags. */
    EXPECT_STR_EQ(message, "HTTP 400: expected , got");
    free(message);
}

static void test_short_html_like_bodies(void)
{
    const char *cases[] = {
        "<",  "<s",  "<sc",     "<scr", "<scri", "<scrip", "<script", "<style", "<style>partial",
        "</", "</s", "</style", "<!",   "<!--",  NULL,
    };
    for (size_t i = 0; cases[i]; i++) {
        char *message = format_api_error(500, cases[i]);
        EXPECT(message != NULL);
        EXPECT(strstr(message, "HTTP 500") != NULL);
        free(message);
    }
}

static void test_html_only_no_text(void)
{
    char *message = format_api_error(502, "<html><head></head><body></body></html>");
    EXPECT_STR_EQ(message, "HTTP 502");
    free(message);
}

static void test_multiline_collapsed(void)
{
    char *message = format_api_error(500, "line1\n\n\nline2\t\ttabbed");
    EXPECT_STR_EQ(message, "HTTP 500: line1 line2 tabbed");
    free(message);
}

static void test_model_list_error_key_rejected(void)
{
    char *message = format_model_list_error("openai", "https://api.openai.com/v1", 1, 401);
    EXPECT_STR_EQ(message, "openai rejected the API key (HTTP 401) — check it and retry");
    free(message);
}

static void test_model_list_error_key_missing(void)
{
    char *message = format_model_list_error("anthropic", "https://api.anthropic.com/v1", 0, 403);
    EXPECT_STR_EQ(message, "anthropic requires an API key (HTTP 403) — none is configured");
    free(message);
}

static void test_model_list_error_empty_2xx(void)
{
    char *message = format_model_list_error("llama.cpp", "http://127.0.0.1:8080/v1", 0, 200);
    EXPECT_STR_EQ(message, "llama.cpp sent an empty or truncated /models response");
    free(message);
}

static void test_model_list_error_http_status(void)
{
    char *message = format_model_list_error("openrouter", "https://openrouter.ai/api/v1", 1, 500);
    EXPECT_STR_EQ(message, "listing openrouter models failed (HTTP 500)");
    free(message);
}

static void test_model_list_error_unreachable(void)
{
    char *message = format_model_list_error("llama.cpp", "http://127.0.0.1:8080/v1", 0, 0);
    EXPECT_STR_EQ(message, "could not reach llama.cpp at http://127.0.0.1:8080/v1");
    free(message);
}

static void test_model_list_error_null_name(void)
{
    char *message = format_model_list_error(NULL, "http://x", 0, 500);
    EXPECT_STR_EQ(message, "listing provider models failed (HTTP 500)");
    free(message);
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
    test_model_list_error_key_rejected();
    test_model_list_error_key_missing();
    test_model_list_error_empty_2xx();
    test_model_list_error_http_status();
    test_model_list_error_unreachable();
    test_model_list_error_null_name();
    T_REPORT();
}
