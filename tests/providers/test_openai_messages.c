/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <string.h>

#include "harness.h"
#include "providers/openai.h"

/* Find the first message with the given role in a built messages array. */
static json_t *find_role(json_t *msgs, const char *role)
{
    size_t i, n = json_array_size(msgs);
    for (i = 0; i < n; i++) {
        json_t *m = json_array_get(msgs, i);
        const char *r = json_string_value(json_object_get(m, "role"));
        if (r && strcmp(r, role) == 0)
            return m;
    }
    return NULL;
}

/* A reasoning item followed by an assistant message + tool call should
 * collapse into one assistant message carrying reasoning_content, content,
 * and tool_calls — when the round-trip field is set. */
static void test_reasoning_attached_when_field_set(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING, .reasoning_text = "let me read it"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Reading the file."},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "c1",
         .tool_name = "read",
         .tool_arguments_json = "{\"path\":\"x\"}"},
    };
    json_t *msgs = openai_build_messages(NULL, items, 3, "reasoning_content");

    EXPECT(json_array_size(msgs) == 1);
    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "reasoning_content")), "let me read it");
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Reading the file.");
    EXPECT(json_array_size(json_object_get(a, "tool_calls")) == 1);

    json_decref(msgs);
}

/* With a NULL field (round-trip disabled), no reasoning_content is emitted
 * even when a reasoning item is present. */
static void test_reasoning_omitted_when_field_null(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING, .reasoning_text = "hidden cot"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Hello."},
    };
    json_t *msgs = openai_build_messages(NULL, items, 2, NULL);

    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_content") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Hello.");

    json_decref(msgs);
}

/* A custom field name (e.g. "reasoning") is honored verbatim. */
static void test_reasoning_custom_field_name(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING, .reasoning_text = "cot"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Hi."},
    };
    json_t *msgs = openai_build_messages(NULL, items, 2, "reasoning");

    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "reasoning")), "cot");
    EXPECT(json_object_get(a, "reasoning_content") == NULL);

    json_decref(msgs);
}

/* Codex-style reasoning (reasoning_json, no reasoning_text) is skipped and
 * never produces a reasoning_content field, regardless of the field setting. */
static void test_codex_reasoning_json_skipped(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING, .reasoning_json = "{\"id\":\"r1\"}"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Done."},
    };
    json_t *msgs = openai_build_messages(NULL, items, 2, "reasoning_content");

    /* The reasoning_json item is its own (skipped) entry; the assistant
     * message stands alone with no reasoning attached. */
    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_content") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Done.");

    json_decref(msgs);
}

/* A reasoning-only turn (the leak case) still emits an assistant message so
 * the CoT round-trips; content is null and there are no tool_calls. */
static void test_reasoning_only_turn(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING, .reasoning_text = "everything leaked here"},
    };
    json_t *msgs = openai_build_messages(NULL, items, 1, "reasoning_content");

    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "reasoning_content")),
                  "everything leaked here");
    EXPECT(json_is_null(json_object_get(a, "content")));
    EXPECT(json_object_get(a, "tool_calls") == NULL);

    json_decref(msgs);
}

/* A reasoning-only turn with replay disabled (field NULL) must NOT emit a
 * bare {"role":"assistant","content":null} — it would poison the next
 * request and some backends reject it. No assistant message at all. */
static void test_reasoning_only_field_null_emits_nothing(void)
{
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = "hi"},
        {.kind = ITEM_REASONING, .reasoning_text = "leaked cot, replay off"},
    };
    json_t *msgs = openai_build_messages(NULL, items, 2, NULL);

    EXPECT(json_array_size(msgs) == 1); /* just the user message */
    EXPECT(find_role(msgs, "assistant") == NULL);

    json_decref(msgs);
}

int main(void)
{
    test_reasoning_attached_when_field_set();
    test_reasoning_omitted_when_field_null();
    test_reasoning_custom_field_name();
    test_codex_reasoning_json_skipped();
    test_reasoning_only_turn();
    test_reasoning_only_field_null_emits_nothing();
    T_REPORT();
}
