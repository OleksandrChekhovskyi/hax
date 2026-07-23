/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
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
        {.kind = ITEM_REASONING,
         .reasoning_text = "let me read it",
         .provider = "llama.cpp",
         .model = "m1"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Reading the file."},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "c1",
         .tool_name = "read",
         .tool_arguments_json = "{\"path\":\"x\"}"},
    };
    json_t *msgs =
        openai_build_messages(NULL, items, 3, "reasoning_content", "llama.cpp", "m1", -1);

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
        {.kind = ITEM_REASONING,
         .reasoning_text = "hidden cot",
         .provider = "llama.cpp",
         .model = "m1"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Hello."},
    };
    json_t *msgs = openai_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", -1);

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
        {.kind = ITEM_REASONING, .reasoning_text = "cot", .provider = "llama.cpp", .model = "m1"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Hi."},
    };
    json_t *msgs = openai_build_messages(NULL, items, 2, "reasoning", "llama.cpp", "m1", -1);

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
    json_t *msgs = openai_build_messages(NULL, items, 2, "reasoning_content", "codex", "o3", -1);

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
        {.kind = ITEM_REASONING,
         .reasoning_text = "everything leaked here",
         .provider = "llama.cpp",
         .model = "m1"},
    };
    json_t *msgs =
        openai_build_messages(NULL, items, 1, "reasoning_content", "llama.cpp", "m1", -1);

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
    json_t *msgs = openai_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", -1);

    EXPECT(json_array_size(msgs) == 1); /* just the user message */
    EXPECT(find_role(msgs, "assistant") == NULL);

    json_decref(msgs);
}

/* Reasoning whose provenance stamp doesn't match the live provider/model is
 * NOT replayed: switching from Codex to llama.cpp (Codex items also carry
 * reasoning_text), or switching the llama.cpp model mid-conversation, must not
 * feed the new backend CoT it never produced. The assistant text/tool_calls
 * still survive — only the reasoning_content is dropped. */
static void test_reasoning_skipped_on_provenance_mismatch(void)
{
    /* Earlier turn produced by codex/o3; now live on llama.cpp/m1. */
    struct item provider_switch[] = {
        {.kind = ITEM_REASONING, .reasoning_text = "codex cot", .provider = "codex", .model = "o3"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "From codex."},
    };
    json_t *msgs =
        openai_build_messages(NULL, provider_switch, 2, "reasoning_content", "llama.cpp", "m1", -1);
    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_content") == NULL); /* stale CoT dropped */
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "From codex.");
    json_decref(msgs);

    /* Same provider, different model: an earlier llama.cpp/m0 turn is stale
     * once the user switches the served model to m1. */
    struct item model_switch[] = {
        {.kind = ITEM_REASONING,
         .reasoning_text = "old model cot",
         .provider = "llama.cpp",
         .model = "m0"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Older model."},
    };
    msgs = openai_build_messages(NULL, model_switch, 2, "reasoning_content", "llama.cpp", "m1", -1);
    a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_content") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Older model.");
    json_decref(msgs);

    /* An unstamped reasoning item (older session record that never got a
     * stamp and no header to backfill from) is conservatively skipped. */
    struct item unstamped[] = {
        {.kind = ITEM_REASONING, .reasoning_text = "no stamp"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Unstamped."},
    };
    msgs = openai_build_messages(NULL, unstamped, 2, "reasoning_content", "llama.cpp", "m1", -1);
    a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_content") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Unstamped.");
    json_decref(msgs);
}

/* A preset that declares an effort ladder surfaces it via list_efforts; one
 * that doesn't reports zero (the /effort picker then skips the step). Both
 * have list_models wired regardless. No network: construction only sets
 * fields, and we never call list_models. */
static void test_list_efforts_wiring(void)
{
    unsetenv("HAX_OPENAI_BASE_URL"); /* use the preset default below */

    struct openai_preset with = {
        .display_name = "withefforts",
        .default_base_url = "http://example.invalid/v1",
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    struct provider *p = openai_provider_new_preset(&with);
    EXPECT(p != NULL);
    if (p) {
        EXPECT(p->list_models != NULL);
        EXPECT(p->list_efforts != NULL);
        const char *const *out = NULL;
        size_t n = p->list_efforts(p, &out);
        EXPECT(n == OPENAI_EFFORT_LADDER_N);
        EXPECT(out != NULL && strcmp(out[0], "none") == 0);
        EXPECT(strcmp(out[n - 1], "xhigh") == 0);
        p->destroy(p);
    }

    struct openai_preset without = {
        .display_name = "noefforts",
        .default_base_url = "http://example.invalid/v1",
    };
    struct provider *q = openai_provider_new_preset(&without);
    EXPECT(q != NULL);
    if (q) {
        const char *const *out = NULL;
        EXPECT(q->list_efforts(q, &out) == 0); /* none → 0, picker skips */
        q->destroy(q);
    }
}

/* A tool result carrying an image part: the `tool` message keeps string
 * content, and the part follows as a separate user message with an
 * image_url data-URL block — placed after the whole run of consecutive
 * tool messages so strict backends still see them adjacent. */
static void test_tool_result_image_followup(void)
{
    struct item_image imgs[] = {
        {.mime = "image/png", .data_b64 = "QUJD", .width = 4, .height = 2},
    };
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT,
         .call_id = "c1",
         .output = "Read image x.png",
         .images = imgs,
         .n_images = 1},
        {.kind = ITEM_TOOL_RESULT, .call_id = "c2", .output = "file1"},
    };
    json_t *msgs = openai_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", 1);

    /* tool, tool, then the image user message — nothing interleaved. */
    EXPECT(json_array_size(msgs) == 3);
    json_t *m0 = json_array_get(msgs, 0);
    json_t *m1 = json_array_get(msgs, 1);
    json_t *m2 = json_array_get(msgs, 2);
    EXPECT_STR_EQ(json_string_value(json_object_get(m0, "role")), "tool");
    EXPECT(json_is_string(json_object_get(m0, "content")));
    EXPECT_STR_EQ(json_string_value(json_object_get(m1, "role")), "tool");
    EXPECT_STR_EQ(json_string_value(json_object_get(m2, "role")), "user");
    json_t *parts = json_object_get(m2, "content");
    EXPECT(json_is_array(parts));
    EXPECT(json_array_size(parts) == 2);
    json_t *img = json_array_get(parts, 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(img, "type")), "image_url");
    const char *url = json_string_value(json_object_get(json_object_get(img, "image_url"), "url"));
    EXPECT_STR_EQ(url, "data:image/png;base64,QUJD");
    json_decref(msgs);

    /* image_input == 0: no follow-up message; the placeholder is appended
     * to the tool message's string content instead. */
    msgs = openai_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", 0);
    EXPECT(json_array_size(msgs) == 2);
    const char *content = json_string_value(json_object_get(json_array_get(msgs, 0), "content"));
    EXPECT(content && strstr(content, "Read image x.png") != NULL);
    EXPECT(strstr(content, "[image:") != NULL);
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
    test_reasoning_skipped_on_provenance_mismatch();
    test_list_efforts_wiring();
    test_tool_result_image_followup();
    T_REPORT();
}
