/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "transport/sse.h"

#define MAX_EVENTS 16

struct captured_event {
    char *event;
    char *data;
};

struct capture_state {
    struct captured_event events[MAX_EVENTS];
    size_t count;
    int stop_after; /* 0 disables the callback limit */
};

static int capture_event(const char *event, const char *data, void *user)
{
    struct capture_state *capture = user;
    if (capture->count >= MAX_EVENTS) {
        FAIL("%s", "too many events captured");
        return 0;
    }
    capture->events[capture->count].event = strdup(event);
    capture->events[capture->count].data = strdup(data);
    capture->count++;
    if (capture->stop_after > 0 && (int)capture->count >= capture->stop_after)
        return 1;
    return 0;
}

static void capture_reset(struct capture_state *capture)
{
    for (size_t i = 0; i < capture->count; i++) {
        free(capture->events[i].event);
        free(capture->events[i].data);
    }
    memset(capture, 0, sizeof(*capture));
}

static void feed_text(struct sse_parser *parser, const char *text)
{
    sse_parser_feed(parser, text, strlen(text));
}

static void test_single_event(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data: hello\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].event, "");
    EXPECT_STR_EQ(capture.events[0].data, "hello");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_event_name(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "event: message\ndata: hi\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].event, "message");
    EXPECT_STR_EQ(capture.events[0].data, "hi");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_multiline_data_joined(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data: line1\ndata: line2\ndata: line3\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "line1\nline2\nline3");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_comment_ignored(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, ": this is a comment\ndata: real\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "real");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_crlf_line_endings(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "event: x\r\ndata: y\r\n\r\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].event, "x");
    EXPECT_STR_EQ(capture.events[0].data, "y");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_no_space_after_colon(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data:x\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "x");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_only_one_leading_space_consumed(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data:  two-spaces\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, " two-spaces");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_unknown_fields_ignored(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "id: 42\nretry: 1000\ndata: payload\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "payload");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_multiple_events(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data: one\n\ndata: two\n\ndata: three\n\n");
    EXPECT(capture.count == 3);
    EXPECT_STR_EQ(capture.events[0].data, "one");
    EXPECT_STR_EQ(capture.events[1].data, "two");
    EXPECT_STR_EQ(capture.events[2].data, "three");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_event_resets_between_events(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "event: a\ndata: 1\n\ndata: 2\n\n");
    EXPECT(capture.count == 2);
    EXPECT_STR_EQ(capture.events[0].event, "a");
    EXPECT_STR_EQ(capture.events[1].event, "");
    EXPECT_STR_EQ(capture.events[1].data, "2");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_byte_by_byte_feeding(void)
{
    const char *input = "event: message\ndata: hi\n\n";
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    for (size_t i = 0; i < strlen(input); i++)
        sse_parser_feed(&parser, input + i, 1);
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].event, "message");
    EXPECT_STR_EQ(capture.events[0].data, "hi");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_chunk_split_mid_line(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data: hel");
    feed_text(&parser, "lo\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "hello");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_finalize_flushes_trailing_event(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data: tail\n");
    EXPECT(capture.count == 0);
    sse_parser_finalize(&parser);
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "tail");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_finalize_flushes_trailing_partial_line(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data: incomplete");
    sse_parser_finalize(&parser);
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "incomplete");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_finalize_with_only_blank_input_emits_nothing(void)
{
    struct capture_state capture = {0};
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    sse_parser_finalize(&parser);
    EXPECT(capture.count == 0);
    capture_reset(&capture);
    sse_parser_free(&parser);
}

static void test_callback_stop_suppresses_further_events(void)
{
    struct capture_state capture = {0};
    capture.stop_after = 1;
    struct sse_parser parser;
    sse_parser_init(&parser, capture_event, &capture);
    feed_text(&parser, "data: a\n\ndata: b\n\ndata: c\n\n");
    EXPECT(capture.count == 1);
    EXPECT_STR_EQ(capture.events[0].data, "a");
    capture_reset(&capture);
    sse_parser_free(&parser);
}

int main(void)
{
    test_single_event();
    test_event_name();
    test_multiline_data_joined();
    test_comment_ignored();
    test_crlf_line_endings();
    test_no_space_after_colon();
    test_only_one_leading_space_consumed();
    test_unknown_fields_ignored();
    test_multiple_events();
    test_event_resets_between_events();
    test_byte_by_byte_feeding();
    test_chunk_split_mid_line();
    test_finalize_flushes_trailing_event();
    test_finalize_flushes_trailing_partial_line();
    test_finalize_with_only_blank_input_emits_nothing();
    test_callback_stop_suppresses_further_events();
    T_REPORT();
}
