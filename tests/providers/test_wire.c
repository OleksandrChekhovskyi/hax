/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>

#include "harness.h"
#include "provider.h"
#include "providers/anthropic_messages.h"
#include "providers/openai_messages.h"
#include "providers/responses_messages.h"
#include "providers/wire.h"

struct capture {
    int n_text;
    char text[64];
};

static int on_event(const struct stream_event *event, void *user)
{
    struct capture *capture = user;
    if (event->kind == EV_TEXT_DELTA) {
        capture->n_text++;
        snprintf(capture->text, sizeof(capture->text), "%s", event->u.text_delta.text);
    }
    return 0;
}

/* Dispatch smoke test per wire; payload translation details live in the parser tests. */
static void expect_text_delta(const struct wire *wire, const char *event_name, const char *data)
{
    struct capture capture = {0};
    union wire_events events;

    wire->events_init(&events, on_event, &capture, NULL);
    wire->events_feed(&events, event_name, data);
    EXPECT(capture.n_text == 1);
    EXPECT_STR_EQ(capture.text, "Hello");
    wire->events_free(&events);
}

static void test_identity(void)
{
    EXPECT_STR_EQ(WIRE_OPENAI_CHAT.id, "openai-completions");
    EXPECT_STR_EQ(WIRE_OPENAI_RESPONSES.id, "openai-responses");
    EXPECT_STR_EQ(WIRE_ANTHROPIC_MESSAGES.id, "anthropic-messages");
    EXPECT_STR_EQ(WIRE_OPENAI_CHAT.path, "/chat/completions");
    EXPECT_STR_EQ(WIRE_OPENAI_RESPONSES.path, "/responses");
    EXPECT_STR_EQ(WIRE_ANTHROPIC_MESSAGES.path, "/messages");
}

static void test_find(void)
{
    EXPECT(wire_find("openai-completions") == &WIRE_OPENAI_CHAT);
    EXPECT(wire_find("chat") == &WIRE_OPENAI_CHAT);
    EXPECT(wire_find("openai-responses") == &WIRE_OPENAI_RESPONSES);
    EXPECT(wire_find("Responses") == &WIRE_OPENAI_RESPONSES);
    EXPECT(wire_find("anthropic-messages") == &WIRE_ANTHROPIC_MESSAGES);
    EXPECT(wire_find("grpc") == NULL);
    EXPECT(wire_find("") == NULL);
    EXPECT(wire_find(NULL) == NULL);
}

static void test_event_dispatch(void)
{
    expect_text_delta(&WIRE_OPENAI_CHAT, NULL,
                      "{\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}");
    expect_text_delta(&WIRE_OPENAI_RESPONSES, NULL,
                      "{\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}");
    expect_text_delta(&WIRE_ANTHROPIC_MESSAGES, "content_block_delta",
                      "{\"type\":\"content_block_delta\",\"index\":0,"
                      "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}");
}

static void test_chat_opts_plumbing(void)
{
    struct capture capture = {0};
    union wire_events events;
    struct wire_events_opts opts = {
        .emit_progress = 1,
        .length_hint = "grow the context",
        .cache_write_1h = 1,
    };

    WIRE_OPENAI_CHAT.events_init(&events, on_event, &capture, &opts);
    EXPECT(events.chat.emit_progress == 1);
    EXPECT_STR_EQ(events.chat.length_hint, "grow the context");
    EXPECT(events.chat.cache_write_1h == 1);
    WIRE_OPENAI_CHAT.events_free(&events);

    /* NULL opts must leave the parser at its defaults. */
    WIRE_OPENAI_CHAT.events_init(&events, on_event, &capture, NULL);
    EXPECT(events.chat.emit_progress == 0);
    EXPECT(events.chat.length_hint == NULL);
    EXPECT(events.chat.cache_write_1h == 0);
    WIRE_OPENAI_CHAT.events_free(&events);
}

/* Body composition is tested with each dialect's builder in its *_messages suite; the wire
 * table only pairs those builders with paths and parsers. */
static void test_build_body_table(void)
{
    EXPECT(WIRE_OPENAI_CHAT.build_body == openai_build_body);
    EXPECT(WIRE_OPENAI_RESPONSES.build_body == responses_build_body);
    EXPECT(WIRE_ANTHROPIC_MESSAGES.build_body == anthropic_build_body);
}

static void test_finish_applies_extra_body(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    json_t *extra = json_pack("{s:f}", "temperature", 0.5);
    struct wire_body_opts opts = {.extra_body = extra};

    char *serialized = wire_build_body(&WIRE_OPENAI_CHAT, &context, "prov", "model-1", &opts);
    EXPECT(serialized != NULL);
    json_t *body = json_loads(serialized, 0, NULL);
    free(serialized);
    EXPECT(body != NULL);
    if (!body) {
        json_decref(extra);
        return;
    }

    EXPECT_STR_EQ(json_string_value(json_object_get(body, "model")), "model-1");
    EXPECT(json_real_value(json_object_get(body, "temperature")) == 0.5);

    json_decref(body);
    json_decref(extra);
}

int main(void)
{
    test_identity();
    test_find();
    test_event_dispatch();
    test_chat_opts_plumbing();
    test_build_body_table();
    test_finish_applies_extra_body();
    T_REPORT();
}
