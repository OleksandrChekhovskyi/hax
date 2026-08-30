/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "harness.h"
#include "provider.h"
#include "util.h"
#include "providers/registry.h"

struct stream_capture {
    struct buf text;
    struct buf reasoning;
    struct buf tool_args;
    char tool_name[64];
    int tool_call_count;
    int done_event_count;
    int error_event_count;
    int callback_result;
    struct stream_usage usage;
};

static void capture_init(struct stream_capture *c)
{
    memset(c, 0, sizeof(*c));
    buf_init(&c->text);
    buf_init(&c->reasoning);
    buf_init(&c->tool_args);
}

static void capture_free(struct stream_capture *c)
{
    buf_free(&c->text);
    buf_free(&c->reasoning);
    buf_free(&c->tool_args);
}

static const char *buf_str(const struct buf *b)
{
    return b->data ? b->data : "";
}

static int capture_cb(const struct stream_event *ev, void *user)
{
    struct stream_capture *c = user;
    switch (ev->kind) {
    case EV_TEXT_DELTA:
        EXPECT(((unsigned char)ev->u.text_delta.text[0] & 0xC0) != 0x80);
        buf_append_str(&c->text, ev->u.text_delta.text);
        break;
    case EV_REASONING_DELTA:
        buf_append_str(&c->reasoning, ev->u.reasoning_delta.text);
        break;
    case EV_TOOL_CALL_START:
        c->tool_call_count++;
        snprintf(c->tool_name, sizeof(c->tool_name), "%s", ev->u.tool_call_start.name);
        break;
    case EV_TOOL_CALL_DELTA:
        buf_append_str(&c->tool_args, ev->u.tool_call_delta.args_delta);
        break;
    case EV_DONE:
        c->done_event_count++;
        c->usage = ev->u.done.usage;
        break;
    case EV_ERROR:
        c->error_event_count++;
        break;
    default:
        break;
    }
    return c->callback_result;
}

static char *write_script(const char *content)
{
    char *path = xasprintf("%s/script.txt", t_tempdir());
    FILE *script = fopen(path, "w");
    if (!script)
        abort();
    int write_failed = fputs(content, script) == EOF;
    int close_failed = fclose(script) == EOF;
    if (write_failed || close_failed)
        abort();
    return path;
}

static struct provider *new_scripted_provider(const char *path)
{
    setenv("HAX_MOCK_SCRIPT", path, 1);
    struct provider *provider = provider_construct(provider_find("mock"));
    unsetenv("HAX_MOCK_SCRIPT");
    return provider;
}

static int capture_stream(struct provider *provider, const struct context *context,
                          struct stream_capture *capture)
{
    static const struct context empty;
    return provider->stream(provider, context ? context : &empty, NULL, capture_cb, capture, NULL,
                            NULL);
}

static const char SCRIPT_FIXTURE[] = "# comment\n"
                                     "text Hello\\nworld\n"
                                     "space\n"
                                     "text again\n"
                                     "reasoning think\\ting\n"
                                     "usage future=99 in=10 out=20 cached=3 cache_write=4 "
                                     "cache_write_1h=2 cost=0.5\n"
                                     "end-turn\n"
                                     "\n"
                                     "tool bash {\"command\":\"ls {{CWD}}\"}\n"
                                     "end-turn\n"
                                     "text ——————————\n"
                                     "end-turn\n";

static void expect_scripted_text_turn(struct provider *provider)
{
    struct stream_capture capture;
    capture_init(&capture);
    EXPECT(capture_stream(provider, NULL, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "Hello\nworld again");
    EXPECT_STR_EQ(buf_str(&capture.reasoning), "think\ting");
    EXPECT(capture.tool_call_count == 0);
    EXPECT(capture.done_event_count == 1);
    EXPECT(capture.usage.input_tokens == 10);
    EXPECT(capture.usage.output_tokens == 20);
    EXPECT(capture.usage.cached_tokens == 3);
    EXPECT(capture.usage.cache_write_tokens == 4);
    EXPECT(capture.usage.cache_write_1h_tokens == 2);
    EXPECT(capture.usage.cost == 0.5);
    capture_free(&capture);
}

static void expect_scripted_tool_turn(struct provider *provider)
{
    struct stream_capture capture;
    capture_init(&capture);
    EXPECT(capture_stream(provider, NULL, &capture) == 0);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        abort();
    char *expected_args = xasprintf("{\"command\":\"ls %s\"}", cwd);
    EXPECT_STR_EQ(capture.tool_name, "bash");
    EXPECT_STR_EQ(buf_str(&capture.tool_args), expected_args);
    EXPECT(capture.tool_call_count == 1);
    EXPECT(capture.done_event_count == 1);
    EXPECT(capture.usage.input_tokens == -1);

    free(expected_args);
    capture_free(&capture);
}

static void expect_scripted_utf8_turn(struct provider *provider)
{
    struct stream_capture capture;
    capture_init(&capture);
    EXPECT(capture_stream(provider, NULL, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "——————————");
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);
}

static void expect_script_exhausted(struct provider *provider)
{
    struct stream_capture capture;
    capture_init(&capture);
    EXPECT(capture_stream(provider, NULL, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "Script exhausted — no more turns.");
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);
}

static void test_scripted_turns(void)
{
    char *path = write_script(SCRIPT_FIXTURE);
    struct provider *provider = new_scripted_provider(path);

    expect_scripted_text_turn(provider);
    expect_scripted_tool_turn(provider);
    expect_scripted_utf8_turn(provider);
    expect_script_exhausted(provider);

    provider->destroy(provider);
    free(path);
}

static int cancel_tick(void *user)
{
    (void)user;
    return 1;
}

static void test_scripted_cancel(void)
{
    char *path = write_script(SCRIPT_FIXTURE);
    struct provider *provider = new_scripted_provider(path);
    struct stream_capture capture;

    static const struct context empty;
    capture_init(&capture);
    EXPECT(provider->stream(provider, &empty, NULL, capture_cb, &capture, cancel_tick, NULL) != 0);
    EXPECT(capture.done_event_count == 0);
    EXPECT(capture.error_event_count == 0);
    capture_free(&capture);

    capture_init(&capture);
    EXPECT(capture_stream(provider, NULL, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "Hello\nworld again");
    capture_free(&capture);

    provider->destroy(provider);
    free(path);
}

static void test_scripted_callback_minus_two_is_abort(void)
{
    char *path = write_script(SCRIPT_FIXTURE);
    struct provider *provider = new_scripted_provider(path);
    struct stream_capture capture;

    capture_init(&capture);
    capture.callback_result = -2;
    EXPECT(capture_stream(provider, NULL, &capture) == -1);
    EXPECT(capture.text.len > 0);
    EXPECT(strstr(buf_str(&capture.text), "Script exhausted") == NULL);
    EXPECT(capture.done_event_count == 0);
    capture_free(&capture);

    expect_scripted_text_turn(provider);

    provider->destroy(provider);
    free(path);
}

static void test_scripted_final_turn_at_eof(void)
{
    char *path = write_script("text final turn\n");
    struct provider *provider = new_scripted_provider(path);
    struct stream_capture capture;

    capture_init(&capture);
    EXPECT(capture_stream(provider, NULL, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "final turn");
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);

    expect_script_exhausted(provider);

    provider->destroy(provider);
    free(path);
}

static void test_scripted_missing_file(void)
{
    char *path = xasprintf("%s/missing.txt", t_tempdir());
    struct provider *provider = new_scripted_provider(path);
    struct stream_capture capture;

    capture_init(&capture);
    EXPECT(capture_stream(provider, NULL, &capture) == -1);
    EXPECT(capture.error_event_count == 1);
    EXPECT(capture.done_event_count == 0);
    capture_free(&capture);

    provider->destroy(provider);
    free(path);
}

static struct provider *new_interactive_provider(void)
{
    unsetenv("HAX_MOCK_SCRIPT");
    return provider_construct(provider_find("mock"));
}

static void test_interactive_bash_call(void)
{
    struct provider *provider = new_interactive_provider();
    struct item items[1] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"run `echo \"hi\" C:\\tmp\n\t\x01`"}};
    struct tool_def tools[1] = {{.name = "bash"}};
    struct context context = {.items = items, .n_items = 1, .tools = tools, .n_tools = 1};
    struct stream_capture capture;

    capture_init(&capture);
    EXPECT(capture_stream(provider, &context, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "Sure, on it.");
    EXPECT_STR_EQ(capture.tool_name, "bash");
    EXPECT_STR_EQ(buf_str(&capture.tool_args),
                  "{\"command\":\"echo \\\"hi\\\" C:\\\\tmp\\n\\t\\u0001\"}");
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);

    provider->destroy(provider);
}

static void test_interactive_read_call(void)
{
    struct provider *provider = new_interactive_provider();
    struct item items[1] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"read `docs/notes.md`"}};
    struct tool_def tools[1] = {{.name = "read"}};
    struct context context = {.items = items, .n_items = 1, .tools = tools, .n_tools = 1};
    struct stream_capture capture;

    capture_init(&capture);
    EXPECT(capture_stream(provider, &context, &capture) == 0);
    EXPECT_STR_EQ(capture.tool_name, "read");
    EXPECT_STR_EQ(buf_str(&capture.tool_args), "{\"path\":\"docs/notes.md\"}");
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);

    provider->destroy(provider);
}

static void test_interactive_echo_without_tools(void)
{
    struct provider *provider = new_interactive_provider();
    struct item items[1] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"run `ls`"}};
    struct context context = {.items = items, .n_items = 1};
    struct stream_capture capture;

    capture_init(&capture);
    EXPECT(capture_stream(provider, &context, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "You said: run `ls`");
    EXPECT(capture.tool_call_count == 0);
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);

    provider->destroy(provider);
}

static void test_interactive_echo_without_matching_tool(void)
{
    struct provider *provider = new_interactive_provider();
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"read `notes.md`"}};
    struct tool_def tools[] = {{.name = "bash"}};
    struct context context = {.items = items, .n_items = 1, .tools = tools, .n_tools = 1};
    struct stream_capture capture;

    capture_init(&capture);
    EXPECT(capture_stream(provider, &context, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "You said: read `notes.md`");
    EXPECT(capture.tool_call_count == 0);
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);

    provider->destroy(provider);
}

static void test_interactive_tool_result_after_boundary(void)
{
    struct provider *provider = new_interactive_provider();
    struct item items[3] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"run `ls`"},
                            {.kind = ITEM_TOOL_RESULT, .output = (char *)"file.c"},
                            {.kind = ITEM_TURN_BOUNDARY}};
    struct tool_def tools[1] = {{.name = "bash"}};
    struct context context = {.items = items, .n_items = 3, .tools = tools, .n_tools = 1};
    struct stream_capture capture;

    capture_init(&capture);
    EXPECT(capture_stream(provider, &context, &capture) == 0);
    EXPECT_STR_EQ(buf_str(&capture.text), "Tool finished — awaiting next instruction.");
    EXPECT(capture.tool_call_count == 0);
    EXPECT(capture.done_event_count == 1);
    capture_free(&capture);

    provider->destroy(provider);
}

int main(void)
{
    test_scripted_turns();
    test_scripted_cancel();
    test_scripted_callback_minus_two_is_abort();
    test_scripted_final_turn_at_eof();
    test_scripted_missing_file();
    test_interactive_bash_call();
    test_interactive_read_call();
    test_interactive_echo_without_tools();
    test_interactive_echo_without_matching_tool();
    test_interactive_tool_result_after_boundary();
    T_REPORT();
}
