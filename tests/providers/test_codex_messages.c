/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <string.h>

#include "harness.h"
#include "providers/codex.h"

static const char *item_type(json_t *item)
{
    return json_string_value(json_object_get(item, "type"));
}

static void test_input_item_shapes(void)
{
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = "hi"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "yo"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "c1",
         .tool_name = "bash",
         .tool_arguments_json = "{\"command\":\"ls\"}"},
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_TOOL_RESULT, .call_id = "c1", .output = "out"},
    };
    json_t *input = codex_build_input_items(items, 5, "codex", "o3", -1);
    EXPECT(json_array_size(input) == 4);

    json_t *user_message = json_array_get(input, 0);
    EXPECT_STR_EQ(item_type(user_message), "message");
    EXPECT_STR_EQ(json_string_value(json_object_get(user_message, "role")), "user");
    json_t *user_content = json_array_get(json_object_get(user_message, "content"), 0);
    EXPECT_STR_EQ(item_type(user_content), "input_text");
    EXPECT_STR_EQ(json_string_value(json_object_get(user_content, "text")), "hi");

    json_t *assistant_message = json_array_get(input, 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(assistant_message, "role")), "assistant");
    json_t *assistant_content = json_array_get(json_object_get(assistant_message, "content"), 0);
    EXPECT_STR_EQ(item_type(assistant_content), "output_text");

    json_t *tool_call = json_array_get(input, 2);
    EXPECT_STR_EQ(item_type(tool_call), "function_call");
    EXPECT_STR_EQ(json_string_value(json_object_get(tool_call, "call_id")), "c1");

    json_t *tool_result = json_array_get(input, 3);
    EXPECT_STR_EQ(item_type(tool_result), "function_call_output");
    EXPECT(json_is_string(json_object_get(tool_result, "output")));
    EXPECT_STR_EQ(json_string_value(json_object_get(tool_result, "output")), "out");
    json_decref(input);
}

static void test_tool_result_image(void)
{
    struct item_image images[] = {
        {.mime = "image/png", .data_b64 = "QUJD", .width = 4, .height = 2},
    };
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT,
         .call_id = "c9",
         .output = "note",
         .images = images,
         .n_images = 1},
    };

    json_t *input = codex_build_input_items(items, 1, "codex", "o3", 1);
    json_t *output = json_object_get(json_array_get(input, 0), "output");
    EXPECT(json_is_array(output));
    EXPECT_STR_EQ(item_type(json_array_get(output, 0)), "input_text");
    json_t *image = json_array_get(output, 1);
    EXPECT_STR_EQ(item_type(image), "input_image");
    EXPECT_STR_EQ(json_string_value(json_object_get(image, "image_url")),
                  "data:image/png;base64,QUJD");
    json_decref(input);

    input = codex_build_input_items(items, 1, "codex", "o3", 0);
    output = json_object_get(json_array_get(input, 0), "output");
    json_t *placeholder = json_array_get(output, 1);
    EXPECT_STR_EQ(item_type(placeholder), "input_text");
    EXPECT(strstr(json_string_value(json_object_get(placeholder, "text")), "[image:") != NULL);
    json_decref(input);
}

static void test_reasoning_provenance(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json =
             "{\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"abc==\"}",
         .provider = "codex",
         .model = "o3"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "done"},
    };

    json_t *input = codex_build_input_items(items, 2, "codex", "o3", -1);
    EXPECT(json_array_size(input) == 2);
    EXPECT_STR_EQ(item_type(json_array_get(input, 0)), "reasoning");
    json_decref(input);

    input = codex_build_input_items(items, 2, "codex", "o4", -1);
    EXPECT(json_array_size(input) == 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(input, 0), "role")),
                  "assistant");
    json_decref(input);

    input = codex_build_input_items(items, 2, "openai", "o3", -1);
    EXPECT(json_array_size(input) == 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(input, 0), "role")),
                  "assistant");
    json_decref(input);
}

int main(void)
{
    test_input_item_shapes();
    test_tool_result_image();
    test_reasoning_provenance();
    T_REPORT();
}
