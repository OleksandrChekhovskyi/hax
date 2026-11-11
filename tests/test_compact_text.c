/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "compact_text.h"
#include "harness.h"
#include "provider.h"
#include "text/utf8.h"

#define ITEMS_MAX 32

struct window {
    struct item items[ITEMS_MAX];
    size_t count;
};

static void add(struct window *window, struct item item)
{
    if (window->count == ITEMS_MAX)
        abort();
    window->items[window->count++] = item;
}

static void add_prompt(struct window *window, const char *text)
{
    add(window, (struct item){.kind = ITEM_TURN_BOUNDARY});
    add(window, (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)text});
}

static void add_tool_turn(struct window *window, const char *output)
{
    add(window, (struct item){.kind = ITEM_TURN_BOUNDARY});
    add(window, (struct item){.kind = ITEM_TOOL_CALL,
                              .call_id = (char *)"c",
                              .tool_name = (char *)"read",
                              .tool_arguments_json = (char *)"{}"});
    add(window,
        (struct item){.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c", .output = (char *)output});
}

/* A prompt and the tool call its first turn makes, as hax records the start of a user turn. */
static void add_prompt_turn(struct window *window, const char *prompt, const char *output)
{
    add_prompt(window, prompt);
    add(window, (struct item){.kind = ITEM_TOOL_CALL,
                              .call_id = (char *)"c",
                              .tool_name = (char *)"read",
                              .tool_arguments_json = (char *)"{}"});
    add(window,
        (struct item){.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c", .output = (char *)output});
}

static char *repeat(const char *unit, size_t times)
{
    size_t length = strlen(unit);
    char *text = malloc(length * times + 1);
    for (size_t i = 0; i < times; i++)
        memcpy(text + i * length, unit, length);
    text[length * times] = '\0';
    return text;
}

static size_t count(const char *text, const char *needle)
{
    size_t found = 0;
    for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle))
        found++;
    return found;
}

static struct compact_text fit(const struct window *window, long max_tokens)
{
    struct compact_text text;
    EXPECT(compact_text_fit(window->items, window->count, max_tokens, &text) == 0);
    return text;
}

static void test_renders_labeled_text(void)
{
    struct item_image image = {
        .mime = (char *)"image/png", .data_b64 = (char *)"AAAA", .width = 2, .height = 2};
    struct window window = {0};
    add(&window, (struct item){.kind = ITEM_USER_MESSAGE,
                               .text = (char *)"old summary",
                               .origin = ITEM_ORIGIN_COMPACT_SEED});
    add(&window, (struct item){.kind = ITEM_TURN_USAGE});
    add_prompt(&window, "fix the bug");
    add(&window, (struct item){.kind = ITEM_REASONING,
                               .reasoning_text = (char *)"thinking",
                               .reasoning_json = (char *)"{}"});
    add(&window, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"Looking."});
    add(&window, (struct item){.kind = ITEM_TOOL_CALL,
                               .call_id = (char *)"c",
                               .tool_name = (char *)"read",
                               .tool_arguments_json = (char *)"{\"path\":\"a.c\"}"});
    add(&window, (struct item){.kind = ITEM_TOOL_RESULT,
                               .call_id = (char *)"c",
                               .output = (char *)"contents",
                               .images = &image,
                               .n_images = 1});
    add(&window, (struct item){.kind = ITEM_TURN_USAGE});

    struct compact_text text = fit(&window, LONG_MAX);
    EXPECT_STR_EQ(text.text, "[earlier summary]\nold summary\n\n"
                             "[user]\nfix the bug\n\n"
                             "[assistant]\nLooking.\n\n"
                             "[tool call: read]\n{\"path\":\"a.c\"}\n\n"
                             "[tool result]\ncontents\n[image: image/png, 2x2, 3 bytes]\n\n");
    EXPECT(text.tokens == text_token_estimate(text.text));
    compact_text_destroy(&text);
}

/* Tool output goes first, and from the oldest turn: one clipped output is enough here. */
static void test_clips_oldest_output_first(void)
{
    char *output = repeat("x", 5000);
    struct window window = {0};
    add_prompt_turn(&window, "first", output);
    add_prompt_turn(&window, "second", output);

    struct compact_text whole = fit(&window, LONG_MAX);
    struct compact_text text = fit(&window, whole.tokens - 1);
    EXPECT(text.tokens < whole.tokens);
    const char *marker = strstr(text.text, "[... 3000 bytes omitted ...]");
    EXPECT(marker && marker < strstr(text.text, "second"));
    EXPECT(count(text.text, "omitted") == 1);

    compact_text_destroy(&text);
    compact_text_destroy(&whole);
    free(output);
}

/* A clipped output keeps whole lines, most of them from its end, as truncated tool results do. */
static void test_clipped_output_keeps_mostly_its_tail(void)
{
    struct buf output;
    buf_init(&output);
    for (int line = 0; line < 500; line++) {
        char text[16];
        snprintf(text, sizeof(text), "line %05d\n", line);
        buf_append_str(&output, text);
    }
    struct window window = {0};
    add_prompt(&window, "run it");
    add_tool_turn(&window, output.data);

    struct compact_text whole = fit(&window, LONG_MAX);
    struct compact_text text = fit(&window, whole.tokens - 1);
    /* 2000 bytes: 250 from the head and 1750 from the tail, each cut back to whole lines. */
    EXPECT(strstr(text.text, "line 00021\n[... 3509 bytes omitted ...]\nline 00341\n") != NULL);
    EXPECT(strstr(text.text, "line 00499\n") != NULL);

    compact_text_destroy(&text);
    compact_text_destroy(&whole);
    buf_free(&output);
}

/* With outputs dropped, call arguments keep only enough to show their target. */
static void test_dropped_outputs_clip_arguments(void)
{
    char *payload = repeat("a", 1000);
    char *arguments = malloc(strlen(payload) + 32);
    sprintf(arguments, "{\"path\": \"todo.py\", \"new\": \"%s\"}", payload);
    char *output = repeat("x", 5000);
    struct window window = {0};
    add_prompt(&window, "go");
    add(&window, (struct item){.kind = ITEM_TOOL_CALL,
                               .call_id = (char *)"c",
                               .tool_name = (char *)"edit",
                               .tool_arguments_json = arguments});
    add(&window, (struct item){.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c", .output = output});

    struct compact_text text = fit(&window, 200);
    EXPECT(strstr(text.text, "{\"path\": \"todo.py\", \"new\": \"aaa") != NULL);
    EXPECT(strstr(text.text, "\n[... 730 bytes omitted ...]\n") != NULL);
    EXPECT(strstr(text.text, "[tool result omitted]") != NULL);
    compact_text_destroy(&text);

    free(output);
    free(arguments);
    free(payload);
}

/* Every turn loses its output detail before any is reduced to its dialogue, which keeps the
 * user's words and the reply that ended the user turn and marks the tool use between them. The
 * newest turn, here with no user text of its own, keeps its call and the fact of its result. */
static void test_marks_what_it_leaves_out(void)
{
    char *output = repeat("x", 5000);
    struct window window = {0};
    add_prompt_turn(&window, "first", output);
    add_tool_turn(&window, output);
    add(&window, (struct item){.kind = ITEM_TURN_BOUNDARY});
    add(&window, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"Done: first."});
    add_prompt_turn(&window, "second", output);
    add_tool_turn(&window, output);

    /* The follow-up turn's tool use is covered by the marker before it. */
    struct compact_text text = fit(&window, 52);
    EXPECT_STR_EQ(text.text, "[user]\nfirst\n\n[assistant's tool use omitted]\n\n"
                             "[assistant]\nDone: first.\n\n"
                             "[user]\nsecond\n\n[assistant's tool use omitted]\n\n"
                             "[tool call: read]\n{}\n\n[tool result omitted]\n\n");
    compact_text_destroy(&text);

    /* Left-out turns leave one marker per gap, counting the empty turns among them. */
    text = fit(&window, 34);
    EXPECT_STR_EQ(text.text, "[3 turns omitted]\n\n"
                             "[user]\nsecond\n\n[assistant's tool use omitted]\n\n"
                             "[tool call: read]\n{}\n\n[tool result omitted]\n\n");
    compact_text_destroy(&text);

    struct compact_text none;
    EXPECT(compact_text_fit(window.items, window.count, 32, &none) == -1);
    EXPECT(none.text == NULL);
    free(output);
}

/* A later user turn started by a prompt the user did not type keeps the latest typed prompt from
 * being the newest, so turns are left out on both sides of it. */
static void test_leaves_gaps_around_latest_prompt(void)
{
    char *output = repeat("x", 5000);
    struct window window = {0};
    add_prompt_turn(&window, "old", output);
    add(&window, (struct item){.kind = ITEM_TURN_BOUNDARY});
    add(&window, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"Done old."});
    add_prompt_turn(&window, "latest", output);
    add_tool_turn(&window, output);
    add(&window, (struct item){.kind = ITEM_TURN_BOUNDARY});
    add(&window, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"Done latest."});
    add(&window, (struct item){.kind = ITEM_TURN_BOUNDARY});
    add(&window, (struct item){.kind = ITEM_USER_MESSAGE,
                               .text = (char *)"continue",
                               .origin = ITEM_ORIGIN_CONTINUATION});
    add(&window, (struct item){.kind = ITEM_TOOL_CALL,
                               .call_id = (char *)"c",
                               .tool_name = (char *)"read",
                               .tool_arguments_json = (char *)"{}"});
    add(&window, (struct item){.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c", .output = output});
    add_tool_turn(&window, output);

    struct compact_text text = fit(&window, 33);
    EXPECT_STR_EQ(text.text, "[2 turns omitted]\n\n"
                             "[user]\nlatest\n\n[assistant's tool use omitted]\n\n"
                             "[3 turns omitted]\n\n"
                             "[tool call: read]\n{}\n\n[tool result omitted]\n\n");
    compact_text_destroy(&text);

    struct compact_text none;
    EXPECT(compact_text_fit(window.items, window.count, 32, &none) == -1);
    free(output);
}

/* A marker longer than the short result it replaces leaves the turn as it was, so a protected turn
 * of short results never grows and older turns make the room. */
static void test_shortening_never_grows_a_turn(void)
{
    char *output = repeat("x", 5000);
    struct window window = {0};
    add_prompt_turn(&window, "first", output);
    add_prompt_turn(&window, "second", "ok");
    add(&window, (struct item){.kind = ITEM_TURN_BOUNDARY});
    for (int i = 0; i < 5; i++) {
        add(&window, (struct item){.kind = ITEM_TOOL_CALL,
                                   .call_id = (char *)"c",
                                   .tool_name = (char *)"read",
                                   .tool_arguments_json = (char *)"{}"});
        add(&window, (struct item){
                         .kind = ITEM_TOOL_RESULT, .call_id = (char *)"c", .output = (char *)"ok"});
    }

    /* The newest turn continues tool use, so it stops at dropped outputs; growing there, it left
     * the text at 78 tokens. */
    struct compact_text text = fit(&window, 75);
    static const char start[] =
        "[1 turn omitted]\n\n[user]\nsecond\n\n[assistant's tool use omitted]\n\n";
    EXPECT(strncmp(text.text, start, sizeof(start) - 1) == 0);
    EXPECT(count(text.text, "[tool result]\nok\n\n") == 5);
    EXPECT(count(text.text, "[tool result omitted]") == 0);
    compact_text_destroy(&text);
    free(output);
}

/* Older user text is clipped, the latest typed prompt never; clipping keeps whole characters. */
static void test_clips_older_prompts_only(void)
{
    char *older = repeat("é", 3000);
    char *latest = repeat("y", 3000);
    char *misaligned = malloc(strlen(older) + 2);
    misaligned[0] = 'a';
    strcpy(misaligned + 1, older);
    struct window window = {0};
    add_prompt(&window, misaligned);
    add_prompt(&window, latest);
    add(&window, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"done"});
    add(&window, (struct item){.kind = ITEM_TURN_USAGE});

    struct compact_text text = fit(&window, 1500);
    EXPECT(strstr(text.text, latest) != NULL);
    EXPECT(count(text.text, "bytes omitted") == 1);
    EXPECT(utf8_is_valid(text.text, strlen(text.text)));
    compact_text_destroy(&text);

    struct compact_text none;
    EXPECT(compact_text_fit(window.items, window.count, 700, &none) == -1);

    free(misaligned);
    free(latest);
    free(older);
}

int main(void)
{
    test_renders_labeled_text();
    test_clips_oldest_output_first();
    test_clipped_output_keeps_mostly_its_tail();
    test_dropped_outputs_clip_arguments();
    test_marks_what_it_leaves_out();
    test_leaves_gaps_around_latest_prompt();
    test_shortening_never_grows_a_turn();
    test_clips_older_prompts_only();
    T_REPORT();
}
