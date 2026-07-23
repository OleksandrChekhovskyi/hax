/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "providers/codex.h"

static const char *type_of(json_t *o)
{
    return json_string_value(json_object_get(o, "type"));
}

/* The flat item kinds map to Responses input entries: messages carry a
 * content array, function_call/_output are flat, boundaries are dropped. */
static void test_codex_basic_shapes(void)
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
    json_t *in = codex_build_input_items(items, 5, "codex", "o3", -1);
    EXPECT(json_array_size(in) == 4); /* boundary dropped */

    json_t *u = json_array_get(in, 0);
    EXPECT_STR_EQ(type_of(u), "message");
    EXPECT_STR_EQ(json_string_value(json_object_get(u, "role")), "user");
    json_t *uc = json_array_get(json_object_get(u, "content"), 0);
    EXPECT_STR_EQ(type_of(uc), "input_text");
    EXPECT_STR_EQ(json_string_value(json_object_get(uc, "text")), "hi");

    json_t *a = json_array_get(in, 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "role")), "assistant");
    EXPECT_STR_EQ(type_of(json_array_get(json_object_get(a, "content"), 0)), "output_text");

    json_t *fc = json_array_get(in, 2);
    EXPECT_STR_EQ(type_of(fc), "function_call");
    EXPECT_STR_EQ(json_string_value(json_object_get(fc, "call_id")), "c1");

    json_t *fo = json_array_get(in, 3);
    EXPECT_STR_EQ(type_of(fo), "function_call_output");
    /* Plain (imageless) result: output is a string. */
    EXPECT(json_is_string(json_object_get(fo, "output")));
    EXPECT_STR_EQ(json_string_value(json_object_get(fo, "output")), "out");
    json_decref(in);
}

/* A tool_result with an image: output becomes an array of content items —
 * input_text for the note, input_image (base64 data URL) for the image, or
 * an input_text placeholder when the model has no image input. */
static void test_codex_tool_result_image(void)
{
    struct item_image imgs[] = {{.mime = "image/png", .data_b64 = "QUJD", .width = 4, .height = 2}};
    struct item items[] = {{.kind = ITEM_TOOL_RESULT,
                            .call_id = "c9",
                            .output = "note",
                            .images = imgs,
                            .n_images = 1}};

    json_t *in = codex_build_input_items(items, 1, "codex", "o3", 1);
    json_t *out = json_object_get(json_array_get(in, 0), "output");
    EXPECT(json_is_array(out));
    EXPECT_STR_EQ(type_of(json_array_get(out, 0)), "input_text");
    json_t *img = json_array_get(out, 1);
    EXPECT_STR_EQ(type_of(img), "input_image");
    EXPECT_STR_EQ(json_string_value(json_object_get(img, "image_url")),
                  "data:image/png;base64,QUJD");
    json_decref(in);

    /* image_input == 0: the image becomes a text placeholder. */
    in = codex_build_input_items(items, 1, "codex", "o3", 0);
    out = json_object_get(json_array_get(in, 0), "output");
    json_t *ph = json_array_get(out, 1);
    EXPECT_STR_EQ(type_of(ph), "input_text");
    EXPECT(strstr(json_string_value(json_object_get(ph, "text")), "[image:") != NULL);
    json_decref(in);
}

/* Encrypted reasoning blobs replay only when their provenance stamp matches
 * the request's provider+model — a switch must not feed the backend CoT it
 * never signed. */
static void test_codex_reasoning_stamp(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"reasoning\",\"id\":\"r1\"}",
         .provider = "codex",
         .model = "o3"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "done"},
    };
    /* Matching stamp: the blob replays ahead of the assistant message. */
    json_t *in = codex_build_input_items(items, 2, "codex", "o3", -1);
    EXPECT(json_array_size(in) == 2);
    EXPECT_STR_EQ(type_of(json_array_get(in, 0)), "reasoning");
    json_decref(in);

    /* Model switched (o3 -> o4): the stale blob is dropped, leaving only the
     * assistant message. */
    in = codex_build_input_items(items, 2, "codex", "o4", -1);
    EXPECT(json_array_size(in) == 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(in, 0), "role")), "assistant");
    json_decref(in);
}

int main(void)
{
    test_codex_basic_shapes();
    test_codex_tool_result_image();
    test_codex_reasoning_stamp();
    T_REPORT();
}
