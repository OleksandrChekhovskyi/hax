/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "providers/anthropic.h"

/* Nth message in the built array. */
static json_t *msg_at(json_t *msgs, size_t i)
{
    return json_array_get(msgs, i);
}

static const char *role_of(json_t *msg)
{
    return json_string_value(json_object_get(msg, "role"));
}

static json_t *content_of(json_t *msg)
{
    return json_object_get(msg, "content");
}

static const char *block_type(json_t *block)
{
    return json_string_value(json_object_get(block, "type"));
}

/* A plain user message becomes role:user with a single text block. */
static void test_user_message(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    json_t *msgs = anthropic_build_messages(items, 1, "anthropic", "m", 0);
    EXPECT(json_array_size(msgs) == 1);
    EXPECT_STR_EQ(role_of(msg_at(msgs, 0)), "user");
    json_t *blocks = content_of(msg_at(msgs, 0));
    EXPECT(json_array_size(blocks) == 1);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "text");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "text")), "hello");
    json_decref(msgs);
}

/* Reasoning (thinking block, valid signature) + assistant text + tool call
 * collapse into one assistant message with blocks ordered thinking, text,
 * tool_use; the tool input parses back into a JSON object. */
static void test_assistant_group_thinking_text_tool(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"reasoned\",\"signature\":\"S\"}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Running it."},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "toolu_1",
         .tool_name = "bash",
         .tool_arguments_json = "{\"cmd\":\"ls\"}"},
    };
    json_t *msgs = anthropic_build_messages(items, 3, "anthropic", "m", 0);
    EXPECT(json_array_size(msgs) == 1);
    json_t *a = msg_at(msgs, 0);
    EXPECT_STR_EQ(role_of(a), "assistant");
    json_t *blocks = content_of(a);
    EXPECT(json_array_size(blocks) == 3);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "signature")), "S");
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 1)), "text");
    json_t *tu = json_array_get(blocks, 2);
    EXPECT_STR_EQ(block_type(tu), "tool_use");
    EXPECT_STR_EQ(json_string_value(json_object_get(tu, "id")), "toolu_1");
    EXPECT_STR_EQ(json_string_value(json_object_get(tu, "name")), "bash");
    json_t *input = json_object_get(tu, "input");
    EXPECT(json_is_object(input));
    EXPECT_STR_EQ(json_string_value(json_object_get(input, "cmd")), "ls");
    json_decref(msgs);
}

/* Empty-signature thinking: downgraded to a text block when the preset
 * disallows empty signatures (real Anthropic), kept as a thinking block when
 * allowed (compat servers). */
static void test_empty_signature_policy(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"cot\",\"signature\":\"\"}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "ok"},
    };

    /* allow_empty_signature = 0 → downgrade thinking to a text block. */
    json_t *strict = anthropic_build_messages(items, 2, "anthropic", "m", 0);
    json_t *sb = content_of(msg_at(strict, 0));
    EXPECT(json_array_size(sb) == 2);
    EXPECT_STR_EQ(block_type(json_array_get(sb, 0)), "text"); /* was thinking */
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(sb, 0), "text")), "cot");
    EXPECT_STR_EQ(block_type(json_array_get(sb, 1)), "text");
    json_decref(strict);

    /* allow_empty_signature = 1 → keep the thinking block verbatim. */
    json_t *loose = anthropic_build_messages(items, 2, "anthropic", "m", 1);
    json_t *lb = content_of(msg_at(loose, 0));
    EXPECT_STR_EQ(block_type(json_array_get(lb, 0)), "thinking");
    json_decref(loose);
}

/* Reasoning whose provenance stamp doesn't match the live provider/model is
 * dropped (a signature is bound to the model that produced it). */
static void test_reasoning_provenance_mismatch_dropped(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"x\",\"signature\":\"S\"}",
         .provider = "anthropic",
         .model = "old-model"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "hi"},
    };
    json_t *msgs = anthropic_build_messages(items, 2, "anthropic", "new-model", 0);
    json_t *blocks = content_of(msg_at(msgs, 0));
    EXPECT(json_array_size(blocks) == 1); /* thinking dropped, only text */
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "text");
    json_decref(msgs);
}

/* Consecutive tool results coalesce into one user message with one
 * tool_result block per result. */
static void test_tool_results_coalesced(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT, .call_id = "a", .output = "out-a"},
        {.kind = ITEM_TOOL_RESULT, .call_id = "b", .output = "out-b"},
    };
    json_t *msgs = anthropic_build_messages(items, 2, "anthropic", "m", 0);
    EXPECT(json_array_size(msgs) == 1);
    json_t *u = msg_at(msgs, 0);
    EXPECT_STR_EQ(role_of(u), "user");
    json_t *blocks = content_of(u);
    EXPECT(json_array_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "tool_result");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "tool_use_id")),
                  "a");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 1), "tool_use_id")),
                  "b");
    json_decref(msgs);
}

/* Redacted thinking is replayed verbatim (provenance permitting). */
static void test_redacted_thinking_replayed(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"redacted_thinking\",\"data\":\"ENC\"}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_TOOL_CALL, .call_id = "t", .tool_name = "x", .tool_arguments_json = "{}"},
    };
    json_t *msgs = anthropic_build_messages(items, 2, "anthropic", "m", 0);
    json_t *blocks = content_of(msg_at(msgs, 0));
    EXPECT(json_array_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "redacted_thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "data")), "ENC");
    json_decref(msgs);
}

/* A tool call with malformed arguments still yields a valid (empty) input
 * object rather than null — the API requires input to be an object. */
static void test_tool_call_bad_args_empty_object(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL,
         .call_id = "t",
         .tool_name = "x",
         .tool_arguments_json = "not json"},
    };
    json_t *msgs = anthropic_build_messages(items, 1, "anthropic", "m", 0);
    json_t *tu = json_array_get(content_of(msg_at(msgs, 0)), 0);
    json_t *input = json_object_get(tu, "input");
    EXPECT(json_is_object(input));
    EXPECT(json_object_size(input) == 0);
    json_decref(msgs);
}

int main(void)
{
    test_user_message();
    test_assistant_group_thinking_text_tool();
    test_empty_signature_policy();
    test_reasoning_provenance_mismatch_dropped();
    test_tool_results_coalesced();
    test_redacted_thinking_replayed();
    test_tool_call_bad_args_empty_object();
    T_REPORT();
}
