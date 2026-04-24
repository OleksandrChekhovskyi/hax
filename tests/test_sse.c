/* SPDX-License-Identifier: MIT */
#include "sse.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"

#define MAX_EVENTS 16

struct captured {
    char *event;
    char *data;
};

struct cap_state {
    struct captured events[MAX_EVENTS];
    size_t n;
    int abort_after; /* 0 = never, N = abort after Nth callback */
};

static int cap_cb(const char *event, const char *data, void *user)
{
    struct cap_state *s = user;
    if (s->n >= MAX_EVENTS) {
        FAIL("%s", "too many events captured");
        return 0;
    }
    s->events[s->n].event = strdup(event);
    s->events[s->n].data = strdup(data);
    s->n++;
    if (s->abort_after > 0 && (int)s->n >= s->abort_after)
        return 1;
    return 0;
}

static void cap_reset(struct cap_state *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->events[i].event);
        free(s->events[i].data);
    }
    memset(s, 0, sizeof(*s));
}

/* Shortcut: feed a whole string, finalize, leave parser to caller to free. */
static void feed_all(struct sse_parser *p, const char *s)
{
    sse_parser_feed(p, s, strlen(s));
}

/* ---------- basic cases ---------- */

static void test_single_event(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data: hello\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].event, "");
    EXPECT_STR_EQ(cap.events[0].data, "hello");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_event_name(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "event: message\ndata: hi\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].event, "message");
    EXPECT_STR_EQ(cap.events[0].data, "hi");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_multiline_data_joined(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data: line1\ndata: line2\ndata: line3\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "line1\nline2\nline3");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_comment_ignored(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, ": this is a comment\ndata: real\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "real");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_crlf_line_endings(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "event: x\r\ndata: y\r\n\r\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].event, "x");
    EXPECT_STR_EQ(cap.events[0].data, "y");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_no_space_after_colon(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data:x\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "x");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_only_one_leading_space_consumed(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data:  two-spaces\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, " two-spaces");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_unknown_fields_ignored(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "id: 42\nretry: 1000\ndata: payload\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "payload");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_multiple_events(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data: one\n\ndata: two\n\ndata: three\n\n");
    EXPECT(cap.n == 3);
    EXPECT_STR_EQ(cap.events[0].data, "one");
    EXPECT_STR_EQ(cap.events[1].data, "two");
    EXPECT_STR_EQ(cap.events[2].data, "three");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_event_resets_between_events(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "event: a\ndata: 1\n\ndata: 2\n\n");
    EXPECT(cap.n == 2);
    EXPECT_STR_EQ(cap.events[0].event, "a");
    /* Second event has no event: field → empty event name again. */
    EXPECT_STR_EQ(cap.events[1].event, "");
    EXPECT_STR_EQ(cap.events[1].data, "2");
    cap_reset(&cap);
    sse_parser_free(&p);
}

/* ---------- boundary behavior ---------- */

static void test_byte_by_byte_feeding(void)
{
    /* Same input as test_event_name but fed one byte at a time. */
    const char *s = "event: message\ndata: hi\n\n";
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    for (size_t i = 0; i < strlen(s); i++)
        sse_parser_feed(&p, s + i, 1);
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].event, "message");
    EXPECT_STR_EQ(cap.events[0].data, "hi");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_chunk_split_mid_line(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data: hel");
    feed_all(&p, "lo\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "hello");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_finalize_flushes_trailing_event(void)
{
    /* Stream ends without a blank line after the last event. */
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data: tail\n");
    EXPECT(cap.n == 0); /* not yet — no blank line */
    sse_parser_finalize(&p);
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "tail");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_finalize_flushes_trailing_partial_line(void)
{
    /* Last line has no newline at all. */
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data: incomplete");
    sse_parser_finalize(&p);
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "incomplete");
    cap_reset(&cap);
    sse_parser_free(&p);
}

static void test_finalize_with_only_blank_input_emits_nothing(void)
{
    struct cap_state cap = {0};
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    sse_parser_finalize(&p);
    EXPECT(cap.n == 0);
    cap_reset(&cap);
    sse_parser_free(&p);
}

/* ---------- callback abort ---------- */

static void test_callback_abort_stops_further_events(void)
{
    struct cap_state cap = {0};
    cap.abort_after = 1;
    struct sse_parser p;
    sse_parser_init(&p, cap_cb, &cap);
    feed_all(&p, "data: a\n\ndata: b\n\ndata: c\n\n");
    EXPECT(cap.n == 1);
    EXPECT_STR_EQ(cap.events[0].data, "a");
    cap_reset(&cap);
    sse_parser_free(&p);
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
    test_callback_abort_stops_further_events();
    T_REPORT();
}
