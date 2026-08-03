/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "config.h"
#include "harness.h"
#include "oneshot.h"
#include "provider.h"
#include "util.h"
#include "transport/http.h"

struct captured_run {
    int result;
    char *out;
    char *err;
};

static char *read_stream(FILE *stream)
{
    EXPECT(fseek(stream, 0, SEEK_END) == 0);
    long length = ftell(stream);
    EXPECT(length >= 0);
    EXPECT(fseek(stream, 0, SEEK_SET) == 0);

    char *text = xmalloc((size_t)length + 1);
    size_t bytes_read = fread(text, 1, (size_t)length, stream);
    text[bytes_read] = '\0';
    return text;
}

static struct captured_run capture_run(struct provider *provider, const char *prompt)
{
    fflush(stdout);
    fflush(stderr);
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    EXPECT(saved_stdout >= 0 && saved_stderr >= 0);
    EXPECT(out != NULL && err != NULL);
    EXPECT(dup2(fileno(out), STDOUT_FILENO) >= 0);
    EXPECT(dup2(fileno(err), STDERR_FILENO) >= 0);

    struct hax_opts options = {.raw = 1};
    int result = oneshot_run(provider, prompt, &options, 4);

    fflush(stdout);
    fflush(stderr);
    EXPECT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    EXPECT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdout);
    close(saved_stderr);

    struct captured_run captured = {
        .result = result,
        .out = read_stream(out),
        .err = read_stream(err),
    };
    fclose(out);
    fclose(err);
    return captured;
}

static void captured_run_free(struct captured_run *run)
{
    free(run->out);
    free(run->err);
}

static int prompt_seen;

static int response_stream(struct provider *provider, const struct context *context,
                           const char *model, stream_cb callback, void *user, http_tick_cb tick,
                           void *tick_user)
{
    (void)provider;
    (void)model;
    (void)tick;
    (void)tick_user;

    for (size_t i = 0; i < context->n_items; i++) {
        if (context->items[i].kind == ITEM_USER_MESSAGE && context->items[i].text &&
            strcmp(context->items[i].text, "hello") == 0)
            prompt_seen = 1;
    }

    struct stream_event events[] = {
        {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "first"}},
        {.kind = EV_REASONING_ITEM, .u.reasoning_item = {.json = "{}"}},
        {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "second\n"}},
        {.kind = EV_DONE,
         .u.done = {.stop_reason = "end_turn",
                    .usage = {.input_tokens = -1,
                              .output_tokens = -1,
                              .cached_tokens = -1,
                              .cache_write_tokens = -1,
                              .cache_write_1h_tokens = -1,
                              .cost = -1}}},
    };
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++)
        if (callback(&events[i], user))
            return -1;
    return 0;
}

static void configure_test_run(void)
{
    config_set_override("model", "test-model");
    config_set_override("preset", "");
    config_set_override("system_prompt", "");
    config_set_override("no_session", "1");
    config_set_override("no_tasks", "1");
    config_set_override("transcript", "");
}

static void test_final_messages_are_pipeable(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = response_stream,
    };

    prompt_seen = 0;
    struct captured_run run = capture_run(&provider, "hello");
    EXPECT(run.result == 0);
    EXPECT(prompt_seen);
    EXPECT_STR_EQ(run.out, "first\nsecond\n");
    EXPECT(strstr(run.err, "hax: test-provider · test-model") != NULL);
    EXPECT(strstr(run.out, "test-provider") == NULL);
    captured_run_free(&run);
}

static void test_missing_model_is_diagnostic(void)
{
    configure_test_run();
    config_set_override("model", CONFIG_VALUE_DEFAULT);
    struct provider provider = {.name = "test-provider", .stream = response_stream};

    struct captured_run run = capture_run(&provider, "hello");
    EXPECT(run.result == 1);
    EXPECT_STR_EQ(run.out, "");
    EXPECT(strstr(run.err, "HAX_MODEL is required") != NULL);
    captured_run_free(&run);
}

int main(void)
{
    test_final_messages_are_pipeable();
    test_missing_model_is_diagnostic();
    config_free();
    T_REPORT();
}
