/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/anthropic_messages.h"

static json_t *message_at(json_t *messages, size_t i)
{
    return json_array_get(messages, i);
}

static const char *message_role(json_t *message)
{
    return json_string_value(json_object_get(message, "role"));
}

static json_t *message_content(json_t *message)
{
    return json_object_get(message, "content");
}

static const char *block_type(json_t *block)
{
    return json_string_value(json_object_get(block, "type"));
}

static void test_user_message(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    json_t *messages = anthropic_build_messages(items, 1, "anthropic", "m", 0, -1);
    EXPECT(json_array_size(messages) == 1);
    EXPECT_STR_EQ(message_role(message_at(messages, 0)), "user");
    json_t *blocks = message_content(message_at(messages, 0));
    EXPECT(json_array_size(blocks) == 1);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "text");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "text")), "hello");
    json_decref(messages);
}

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
    json_t *messages = anthropic_build_messages(items, 3, "anthropic", "m", 0, -1);
    EXPECT(json_array_size(messages) == 1);
    json_t *assistant = message_at(messages, 0);
    EXPECT_STR_EQ(message_role(assistant), "assistant");
    json_t *blocks = message_content(assistant);
    EXPECT(json_array_size(blocks) == 3);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "signature")), "S");
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 1)), "text");
    json_t *tool_use = json_array_get(blocks, 2);
    EXPECT_STR_EQ(block_type(tool_use), "tool_use");
    EXPECT_STR_EQ(json_string_value(json_object_get(tool_use, "id")), "toolu_1");
    EXPECT_STR_EQ(json_string_value(json_object_get(tool_use, "name")), "bash");
    json_t *input = json_object_get(tool_use, "input");
    EXPECT(json_is_object(input));
    EXPECT_STR_EQ(json_string_value(json_object_get(input, "cmd")), "ls");
    json_decref(messages);
}

static void test_empty_signature_policy(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"cot\",\"signature\":\"\"}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "ok"},
    };

    json_t *strict = anthropic_build_messages(items, 2, "anthropic", "m", 0, -1);
    json_t *strict_content = message_content(message_at(strict, 0));
    EXPECT(json_array_size(strict_content) == 2);
    EXPECT_STR_EQ(block_type(json_array_get(strict_content, 0)), "text");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(strict_content, 0), "text")),
                  "cot");
    EXPECT_STR_EQ(block_type(json_array_get(strict_content, 1)), "text");
    json_decref(strict);

    json_t *loose = anthropic_build_messages(items, 2, "anthropic", "m", 1, -1);
    json_t *compat_content = message_content(message_at(loose, 0));
    EXPECT_STR_EQ(block_type(json_array_get(compat_content, 0)), "thinking");
    json_decref(loose);
}

static void test_reasoning_provenance_mismatch_dropped(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"x\",\"signature\":\"S\"}",
         .provider = "anthropic",
         .model = "old-model"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "hi"},
    };
    json_t *messages = anthropic_build_messages(items, 2, "anthropic", "new-model", 0, -1);
    json_t *blocks = message_content(message_at(messages, 0));
    EXPECT(json_array_size(blocks) == 1);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "text");
    json_decref(messages);
}

static void test_tool_results_coalesced(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT, .call_id = "a", .output = "out-a"},
        {.kind = ITEM_TOOL_RESULT, .call_id = "b", .output = "out-b"},
    };
    json_t *messages = anthropic_build_messages(items, 2, "anthropic", "m", 0, -1);
    EXPECT(json_array_size(messages) == 1);
    json_t *user = message_at(messages, 0);
    EXPECT_STR_EQ(message_role(user), "user");
    json_t *blocks = message_content(user);
    EXPECT(json_array_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "tool_result");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "tool_use_id")),
                  "a");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 1), "tool_use_id")),
                  "b");
    json_decref(messages);
}

static void test_redacted_thinking_replayed(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"redacted_thinking\",\"data\":\"ENC\"}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_TOOL_CALL, .call_id = "t", .tool_name = "x", .tool_arguments_json = "{}"},
    };
    json_t *messages = anthropic_build_messages(items, 2, "anthropic", "m", 0, -1);
    json_t *blocks = message_content(message_at(messages, 0));
    EXPECT(json_array_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "redacted_thinking");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(blocks, 0), "data")), "ENC");
    json_decref(messages);
}

static void test_tool_call_bad_args_empty_object(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL,
         .call_id = "t",
         .tool_name = "x",
         .tool_arguments_json = "not json"},
    };
    json_t *messages = anthropic_build_messages(items, 1, "anthropic", "m", 0, -1);
    json_t *tool_use = json_array_get(message_content(message_at(messages, 0)), 0);
    json_t *input = json_object_get(tool_use, "input");
    EXPECT(json_is_object(input));
    EXPECT(json_object_size(input) == 0);
    json_decref(messages);
}

static void test_tool_result_image(void)
{
    struct item_image images[] = {
        {.mime = "image/png", .data_b64 = "QUJD", .width = 4, .height = 2},
    };
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT,
         .call_id = "toolu_9",
         .output = "Read image x.png",
         .images = images,
         .n_images = 1},
    };

    json_t *messages = anthropic_build_messages(items, 1, "anthropic", "m", 0, 1);
    json_t *content = message_content(message_at(messages, 0));
    json_t *tool_result = json_array_get(content, 0);
    EXPECT_STR_EQ(block_type(tool_result), "tool_result");
    json_t *blocks = json_object_get(tool_result, "content");
    EXPECT(json_is_array(blocks));
    EXPECT(json_array_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(json_array_get(blocks, 0)), "text");
    json_t *image = json_array_get(blocks, 1);
    EXPECT_STR_EQ(block_type(image), "image");
    json_t *src = json_object_get(image, "source");
    EXPECT_STR_EQ(json_string_value(json_object_get(src, "type")), "base64");
    EXPECT_STR_EQ(json_string_value(json_object_get(src, "media_type")), "image/png");
    EXPECT_STR_EQ(json_string_value(json_object_get(src, "data")), "QUJD");
    json_decref(messages);

    messages = anthropic_build_messages(items, 1, "anthropic", "m", 0, 0);
    tool_result = json_array_get(message_content(message_at(messages, 0)), 0);
    blocks = json_object_get(tool_result, "content");
    EXPECT(json_array_size(blocks) == 2);
    json_t *placeholder = json_array_get(blocks, 1);
    EXPECT_STR_EQ(block_type(placeholder), "text");
    const char *text = json_string_value(json_object_get(placeholder, "text"));
    EXPECT(text && strstr(text, "[image:") != NULL);
    EXPECT(strstr(text, "image/png") != NULL);
    json_decref(messages);

    struct item plain[] = {
        {.kind = ITEM_TOOL_RESULT, .call_id = "toolu_9", .output = "ok"},
    };
    messages = anthropic_build_messages(plain, 1, "anthropic", "m", 0, 1);
    tool_result = json_array_get(message_content(message_at(messages, 0)), 0);
    EXPECT(json_is_string(json_object_get(tool_result, "content")));
    json_decref(messages);
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
    test_tool_result_image();
    T_REPORT();
}
