/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <string.h>

#include "catalog.h"
#include "harness.h"
#include "provider.h"
#include "providers/chat_body.h"
#include "providers/wire.h"

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
    json_t *msgs = chat_build_messages(NULL, items, 3, "reasoning_content", "llama.cpp", "m1", -1);

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
    json_t *msgs = chat_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", -1);

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
    json_t *msgs = chat_build_messages(NULL, items, 2, "reasoning", "llama.cpp", "m1", -1);

    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "reasoning")), "cot");
    EXPECT(json_object_get(a, "reasoning_content") == NULL);

    json_decref(msgs);
}

/* Another wire's opaque reasoning encoding (an unstamped Responses item) never reaches a Chat
 * Completions message, in either the text member or the typed sequence. */
static void test_codex_reasoning_json_ignored(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING, .reasoning_json = "{\"id\":\"r1\"}"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Done."},
    };
    json_t *msgs = chat_build_messages(NULL, items, 2, "reasoning_content", "codex", "o3", -1);

    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_content") == NULL);
    EXPECT(json_object_get(a, "reasoning_details") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Done.");

    json_decref(msgs);
}

/* Typed reasoning blocks are opaque replay state the backend requires back verbatim, so they
 * round-trip independently of the text member — including when the model exposed no plaintext
 * at all, and across the several items one assistant message may span. */
static void test_reasoning_details_round_trip(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "[{\"type\":\"reasoning.encrypted\",\"data\":\"aa\"}]",
         .provider = "openrouter",
         .model = "m1"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "c1",
         .tool_name = "read",
         .tool_arguments_json = "{\"path\":\"x\"}"},
        {.kind = ITEM_REASONING,
         .reasoning_json = "[{\"type\":\"reasoning.encrypted\",\"data\":\"bb\"}]",
         .provider = "openrouter",
         .model = "m1"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Done."},
    };
    json_t *msgs = chat_build_messages(NULL, items, 4, NULL, "openrouter", "m1", -1);

    EXPECT(json_array_size(msgs) == 1);
    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    json_t *details = json_object_get(a, "reasoning_details");
    EXPECT(json_array_size(details) == 2);
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 0), "data")), "aa");
    EXPECT_STR_EQ(json_string_value(json_object_get(json_array_get(details, 1), "data")), "bb");
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Done.");

    json_decref(msgs);
}

/* The typed sequence carries the same reasoning as the plain member, so only one of them is
 * sent; and reasoning from another provider or model is replayed by neither. */
static void test_reasoning_details_supersede_text(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_text = "thinking",
         .reasoning_json = "[{\"type\":\"reasoning.text\",\"text\":\"thinking\"}]",
         .provider = "openrouter",
         .model = "m1"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Done."},
    };
    json_t *msgs = chat_build_messages(NULL, items, 2, "reasoning", "openrouter", "m1", -1);
    json_t *a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_array_size(json_object_get(a, "reasoning_details")) == 1);
    EXPECT(json_object_get(a, "reasoning") == NULL);
    json_decref(msgs);

    msgs = chat_build_messages(NULL, items, 2, "reasoning", "openrouter", "m2", -1);
    a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_details") == NULL);
    EXPECT(json_object_get(a, "reasoning") == NULL);
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
    json_t *msgs = chat_build_messages(NULL, items, 1, "reasoning_content", "llama.cpp", "m1", -1);

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
    json_t *msgs = chat_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", -1);

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
        chat_build_messages(NULL, provider_switch, 2, "reasoning_content", "llama.cpp", "m1", -1);
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
    msgs = chat_build_messages(NULL, model_switch, 2, "reasoning_content", "llama.cpp", "m1", -1);
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
    msgs = chat_build_messages(NULL, unstamped, 2, "reasoning_content", "llama.cpp", "m1", -1);
    a = find_role(msgs, "assistant");
    EXPECT(a != NULL);
    EXPECT(json_object_get(a, "reasoning_content") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(a, "content")), "Unstamped.");
    json_decref(msgs);
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
    json_t *msgs = chat_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", 1);

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
    msgs = chat_build_messages(NULL, items, 2, NULL, "llama.cpp", "m1", 0);
    EXPECT(json_array_size(msgs) == 2);
    const char *content = json_string_value(json_object_get(json_array_get(msgs, 0), "content"));
    EXPECT(content && strstr(content, "Read image x.png") != NULL);
    EXPECT(strstr(content, "[image:") != NULL);
    json_decref(msgs);
}

/* ---------- prompt cache breakpoints ---------- */

/* The cache_control marker on a message's last content part, or NULL. */
static json_t *breakpoint_of(json_t *msg)
{
    json_t *content = json_object_get(msg, "content");
    if (!json_is_array(content) || json_array_size(content) == 0)
        return NULL;
    json_t *last = json_array_get(content, json_array_size(content) - 1);
    return json_object_get(last, "cache_control");
}

static int count_breakpoints(json_t *msgs)
{
    int n = 0;
    for (size_t i = 0; i < json_array_size(msgs); i++)
        if (breakpoint_of(json_array_get(msgs, i)))
            n++;
    return n;
}

static void test_cache_breakpoints_system_and_tail(void)
{
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"first"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"reply"},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"second"},
    };
    json_t *msgs = chat_build_messages("sys", items, 3, NULL, "openrouter", "m", -1);
    chat_apply_cache_breakpoints(msgs, "1h");

    /* Exactly two: the stable prefix and the rolling tail. */
    EXPECT(count_breakpoints(msgs) == 2);
    json_t *sys = json_array_get(msgs, 0);
    EXPECT(breakpoint_of(sys) != NULL);
    /* String content is promoted to the one-part array form, text intact. */
    json_t *part = json_array_get(json_object_get(sys, "content"), 0);
    EXPECT_STR_EQ(json_string_value(json_object_get(part, "type")), "text");
    EXPECT_STR_EQ(json_string_value(json_object_get(part, "text")), "sys");
    json_t *cc = breakpoint_of(sys);
    EXPECT_STR_EQ(json_string_value(json_object_get(cc, "type")), "ephemeral");
    EXPECT_STR_EQ(json_string_value(json_object_get(cc, "ttl")), "1h");
    /* The tail is the last message, not the last *user* one. */
    EXPECT(breakpoint_of(json_array_get(msgs, json_array_size(msgs) - 1)) != NULL);
    json_decref(msgs);
}

static void test_cache_breakpoint_lands_on_tool_result(void)
{
    /* The common agentic shape: the request ends on tool results. Stopping
     * at the last user message would leave every tool result uncached —
     * the bulk of the prefix an agent re-sends each turn. */
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"go"},
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"bash"},
        {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c1", .output = (char *)"output"},
    };
    json_t *msgs = chat_build_messages(NULL, items, 3, NULL, "openrouter", "m", -1);
    chat_apply_cache_breakpoints(msgs, "5m");
    json_t *last = json_array_get(msgs, json_array_size(msgs) - 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(last, "role")), "tool");
    EXPECT(breakpoint_of(last) != NULL);
    /* No system message here, so the tail carries the only breakpoint. */
    EXPECT(count_breakpoints(msgs) == 1);
    /* 5m is the wire default: the TTL field is omitted entirely. */
    EXPECT(json_object_get(breakpoint_of(last), "ttl") == NULL);
    json_decref(msgs);
}

static void test_cache_breakpoint_skips_contentless_assistant(void)
{
    /* An assistant turn that was nothing but tool calls has content:null
     * and no part to mark — the breakpoint walks back to a message that
     * can hold one rather than being dropped. */
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"go"},
        {.kind = ITEM_TOOL_CALL, .call_id = (char *)"c1", .tool_name = (char *)"bash"},
    };
    json_t *msgs = chat_build_messages(NULL, items, 2, NULL, "openrouter", "m", -1);
    json_t *last = json_array_get(msgs, json_array_size(msgs) - 1);
    EXPECT(json_is_null(json_object_get(last, "content")));
    chat_apply_cache_breakpoints(msgs, "1h");
    EXPECT(breakpoint_of(last) == NULL);
    EXPECT(breakpoint_of(json_array_get(msgs, 0)) != NULL); /* the user message */
    EXPECT(count_breakpoints(msgs) == 1);
    json_decref(msgs);
}

static void test_cache_breakpoint_system_only(void)
{
    /* Nothing but a system prompt: it takes the breakpoint once, and the
     * tail pass must not double-mark it. */
    json_t *msgs = chat_build_messages("sys", NULL, 0, NULL, "openrouter", "m", -1);
    chat_apply_cache_breakpoints(msgs, "1h");
    EXPECT(json_array_size(msgs) == 1);
    EXPECT(count_breakpoints(msgs) == 1);
    json_decref(msgs);
}

static json_t *encode_reasoning(enum chat_reasoning_format format, const char *effort)
{
    json_t *body = json_object();
    chat_apply_reasoning(body, format, effort);
    return body;
}

static void test_reasoning_format_parse(void)
{
    EXPECT(chat_reasoning_format_parse("flat", CHAT_REASONING_NESTED) == CHAT_REASONING_FLAT);
    EXPECT(chat_reasoning_format_parse("NESTED", CHAT_REASONING_FLAT) == CHAT_REASONING_NESTED);
    EXPECT(chat_reasoning_format_parse(NULL, CHAT_REASONING_NESTED) == CHAT_REASONING_NESTED);
    EXPECT(chat_reasoning_format_parse("", CHAT_REASONING_FLAT) == CHAT_REASONING_FLAT);
    EXPECT(chat_reasoning_format_parse("invalid", CHAT_REASONING_NESTED) == CHAT_REASONING_NESTED);
}

static void test_reasoning_effort_unset_omitted(void)
{
    const char *empties[] = {NULL, ""};
    for (size_t i = 0; i < 2; i++) {
        json_t *flat = encode_reasoning(CHAT_REASONING_FLAT, empties[i]);
        json_t *nested = encode_reasoning(CHAT_REASONING_NESTED, empties[i]);
        EXPECT(json_object_size(flat) == 0);
        EXPECT(json_object_size(nested) == 0);
        json_decref(flat);
        json_decref(nested);
    }
}

static void test_reasoning_effort_flat(void)
{
    json_t *body = encode_reasoning(CHAT_REASONING_FLAT, "high");
    EXPECT(json_object_get(body, "reasoning") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(body, "reasoning_effort")), "high");
    json_decref(body);

    body = encode_reasoning(CHAT_REASONING_FLAT, "none");
    EXPECT_STR_EQ(json_string_value(json_object_get(body, "reasoning_effort")), "none");
    json_decref(body);
}

static void test_reasoning_effort_nested(void)
{
    json_t *body = encode_reasoning(CHAT_REASONING_NESTED, "high");
    EXPECT(json_object_get(body, "reasoning_effort") == NULL);
    json_t *reasoning = json_object_get(body, "reasoning");
    EXPECT(json_is_object(reasoning));
    EXPECT(json_is_true(json_object_get(reasoning, "enabled")));
    EXPECT_STR_EQ(json_string_value(json_object_get(reasoning, "effort")), "high");
    json_decref(body);
}

static void test_reasoning_effort_nested_none_disables(void)
{
    json_t *body = encode_reasoning(CHAT_REASONING_NESTED, "none");
    json_t *reasoning = json_object_get(body, "reasoning");
    EXPECT(json_is_object(reasoning));
    EXPECT(json_is_false(json_object_get(reasoning, "enabled")));
    EXPECT(json_object_get(reasoning, "effort") == NULL);
    json_decref(body);
}

static void test_build_body_composition(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct tool_def tools[] = {{.name = "read", .description = "read a file"}};
    struct context context = {
        .system_prompt = "be brief",
        .items = items,
        .n_items = 1,
        .tools = tools,
        .n_tools = 1,
        .effort = "high",
        .image_input = 1,
    };
    struct wire_body_opts opts = {
        .cache_markers = 1,
        .cache_ttl = "1h",
        .session_cache_key = "sess-1",
        .reasoning_format = CHAT_REASONING_FLAT,
        .request_cost = 1,
    };

    json_t *body = chat_build_body(&context, "prov", "model-1", &opts);
    EXPECT_STR_EQ(json_string_value(json_object_get(body, "model")), "model-1");
    EXPECT(json_object_get(body, "stream") == json_true());
    EXPECT(json_is_true(json_object_get(json_object_get(body, "stream_options"), "include_usage")));

    /* Chat Completions nests function schemas, unlike the flat Responses declaration. */
    json_t *tool = json_array_get(json_object_get(body, "tools"), 0);
    EXPECT_STR_EQ(json_string_value(json_object_get(json_object_get(tool, "function"), "name")),
                  "read");

    EXPECT_STR_EQ(json_string_value(json_object_get(body, "prompt_cache_key")), "sess-1");
    EXPECT(json_is_true(json_object_get(json_object_get(body, "usage"), "include")));
    EXPECT_STR_EQ(json_string_value(json_object_get(body, "reasoning_effort")), "high");

    /* The system prompt leads the messages array and carries a cache breakpoint. */
    json_t *first = json_array_get(json_object_get(body, "messages"), 0);
    EXPECT_STR_EQ(json_string_value(json_object_get(first, "role")), "system");
    EXPECT(breakpoint_of(first) != NULL);

    json_decref(body);
}

static void test_cache_plan_follows_model_rates(void)
{
    struct catalog_entry rates;

    /* Anthropic-style rates: writes replace input processing, and a 1h rate is quoted. */
    catalog_entry_init(&rates);
    rates.cost_input = 3;
    rates.cost_output = 15;
    rates.cost_cache_write = 3.75;
    rates.cost_cache_write_1h = 6;

    struct chat_cache_plan plan = chat_plan_cache(&rates, CHAT_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 1);

    plan = chat_plan_cache(&rates, CHAT_CACHE_AUTO, "5m");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);

    plan = chat_plan_cache(&rates, CHAT_CACHE_OFF, "1h");
    EXPECT(plan.send_breakpoints == 0);
    EXPECT(plan.writes_bill_1h == 0);

    /* A write rate without a quoted 1h rate must not use the 1h billing fallback. */
    catalog_entry_init(&rates);
    rates.cost_input = 1;
    rates.cost_output = 6;
    rates.cost_cache_write = 1.25;

    plan = chat_plan_cache(&rates, CHAT_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);

    /* A cache-write surcharge does not replace input processing, so AUTO declines it. */
    catalog_entry_init(&rates);
    rates.cost_input = 2;
    rates.cost_output = 12;
    rates.cost_cache_read = 0.2;
    rates.cost_cache_write = 0.375;

    plan = chat_plan_cache(&rates, CHAT_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 0);
    EXPECT(plan.writes_bill_1h == 0);

    plan = chat_plan_cache(&rates, CHAT_CACHE_ON, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);

    /* Unknown rates use the more common replacement policy, so AUTO opts in. */
    catalog_entry_init(&rates);
    plan = chat_plan_cache(&rates, CHAT_CACHE_AUTO, "1h");
    EXPECT(plan.send_breakpoints == 1);
    EXPECT(plan.writes_bill_1h == 0);
}

static void test_build_body_minimal_opts(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    struct wire_body_opts opts = {0};

    json_t *body = chat_build_body(&context, "prov", "model-1", &opts);
    EXPECT(json_object_get(body, "tools") == NULL);
    EXPECT(json_object_get(body, "prompt_cache_key") == NULL);
    EXPECT(json_object_get(body, "return_progress") == NULL);
    EXPECT(json_object_get(body, "usage") == NULL);
    EXPECT(json_object_get(body, "reasoning_effort") == NULL);
    EXPECT(count_breakpoints(json_object_get(body, "messages")) == 0);
    json_decref(body);
}

int main(void)
{
    test_reasoning_format_parse();
    test_reasoning_effort_unset_omitted();
    test_reasoning_effort_flat();
    test_reasoning_effort_nested();
    test_reasoning_effort_nested_none_disables();
    test_cache_breakpoints_system_and_tail();
    test_cache_breakpoint_lands_on_tool_result();
    test_cache_breakpoint_skips_contentless_assistant();
    test_cache_breakpoint_system_only();
    test_reasoning_attached_when_field_set();
    test_reasoning_omitted_when_field_null();
    test_reasoning_custom_field_name();
    test_codex_reasoning_json_ignored();
    test_reasoning_details_round_trip();
    test_reasoning_details_supersede_text();
    test_reasoning_only_turn();
    test_reasoning_only_field_null_emits_nothing();
    test_reasoning_skipped_on_provenance_mismatch();
    test_tool_result_image_followup();
    test_cache_plan_follows_model_rates();
    test_build_body_composition();
    test_build_body_minimal_opts();
    T_REPORT();
}
