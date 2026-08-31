/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "config.h"
#include "harness.h"
#include "oneshot.h"
#include "provider.h"
#include "xalloc.h"
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

static struct captured_run capture_run(struct provider *provider, const char *prompt,
                                       const struct hax_opts *options)
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

    int result = oneshot_run(provider, prompt, options);

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
    /* A HAX_PROVIDER inherited from the environment (e.g. a hax-driven run) would otherwise
     * override the fake provider's identity in session records. */
    config_set_override("provider", "");
    config_set_override("model", "test-model");
    config_set_override("max_turns", "4");
    config_set_override("preset", "");
    config_set_override("system_prompt", "");
    config_set_override("no_session", "1");
    config_set_override("no_tasks", "1");
    config_set_override("transcript", "");
    /* Never fork real power-management helpers (caffeinate / systemd-inhibit). */
    config_set_override("keep_awake", "0");
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
    struct hax_opts options = {.raw = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 0);
    EXPECT(prompt_seen);
    EXPECT_STR_EQ(run.out, "first\nsecond\n");
    EXPECT(strstr(run.err, "hax: test-provider · test-model") != NULL);
    EXPECT(strstr(run.out, "test-provider") == NULL);
    captured_run_free(&run);
}

static void test_json_streams_records_and_result(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = response_stream,
    };

    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 0);
    /* The stream owns the banner's and stats' content; stderr keeps only diagnostics. */
    EXPECT_STR_EQ(run.err, "");

    int saw_user = 0;
    int saw_assistant = 0;
    size_t line_number = 0;
    json_t *last = NULL;
    char *save = NULL;
    for (char *line = strtok_r(run.out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        json_t *record = json_loads(line, 0, NULL);
        EXPECT(json_is_object(record));
        if (!record)
            continue;

        if (line_number++ == 0) {
            EXPECT_STR_EQ(json_string_value(json_object_get(record, "type")), "session");
            EXPECT_STR_EQ(json_string_value(json_object_get(record, "provider")), "test-provider");
            EXPECT_STR_EQ(json_string_value(json_object_get(record, "model")), "test-model");
        }
        const char *kind = json_string_value(json_object_get(record, "kind"));
        const char *item_text = json_string_value(json_object_get(record, "text"));
        if (kind && strcmp(kind, "user") == 0 && item_text && strcmp(item_text, "hello") == 0)
            saw_user = 1;
        if (kind && strcmp(kind, "assistant") == 0)
            saw_assistant = 1;

        if (last)
            json_decref(last);
        last = record;
    }

    EXPECT(saw_user);
    EXPECT(saw_assistant);
    EXPECT(last != NULL);
    if (last) {
        EXPECT_STR_EQ(json_string_value(json_object_get(last, "type")), "result");
        EXPECT_STR_EQ(json_string_value(json_object_get(last, "outcome")), "complete");
        EXPECT_STR_EQ(json_string_value(json_object_get(last, "text")), "first\nsecond\n");
        EXPECT(json_integer_value(json_object_get(last, "turns")) == 1);
        json_decref(last);
    }
    captured_run_free(&run);
}

static int error_stream(struct provider *provider, const struct context *context, const char *model,
                        stream_cb callback, void *user, http_tick_cb tick, void *tick_user)
{
    (void)provider;
    (void)context;
    (void)model;
    (void)tick;
    (void)tick_user;

    struct stream_event event = {.kind = EV_ERROR, .u.error = {.message = "scripted failure"}};
    callback(&event, user);
    return -1;
}

static void test_json_reports_provider_error(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = error_stream,
    };

    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 1);
    EXPECT(strstr(run.err, "provider error: scripted failure") != NULL);

    char *line = run.out;
    char *next;
    while ((next = strchr(line, '\n')) && next[1])
        line = next + 1;
    json_t *last = json_loads(line, 0, NULL);
    EXPECT(json_is_object(last));
    if (last) {
        EXPECT_STR_EQ(json_string_value(json_object_get(last, "type")), "result");
        EXPECT_STR_EQ(json_string_value(json_object_get(last, "outcome")), "error");
        EXPECT_STR_EQ(json_string_value(json_object_get(last, "error")), "scripted failure");
        EXPECT(json_object_get(last, "text") == NULL);
        json_decref(last);
    }
    captured_run_free(&run);
}

/* Run with stdout as a pipe whose reader already exited, under the default SIGPIPE
 * disposition: the run itself must neutralize SIGPIPE so the write surfaces as a checked
 * error instead of killing the process before task cleanup. */
static struct captured_run capture_run_broken_stdout(struct provider *provider, const char *prompt,
                                                     const struct hax_opts *options)
{
    void (*saved_sigpipe)(int) = signal(SIGPIPE, SIG_DFL);
    int pipe_fds[2];
    EXPECT(pipe(pipe_fds) == 0);
    close(pipe_fds[0]);

    fflush(stdout);
    fflush(stderr);
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    FILE *err = tmpfile();
    EXPECT(saved_stdout >= 0 && saved_stderr >= 0 && err != NULL);
    EXPECT(dup2(pipe_fds[1], STDOUT_FILENO) >= 0);
    EXPECT(dup2(fileno(err), STDERR_FILENO) >= 0);

    int result = oneshot_run(provider, prompt, options);

    fflush(stderr);
    EXPECT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    EXPECT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    clearerr(stdout);
    close(saved_stdout);
    close(saved_stderr);
    close(pipe_fds[1]);
    signal(SIGPIPE, saved_sigpipe);

    struct captured_run captured = {
        .result = result,
        .out = xstrdup(""),
        .err = read_stream(err),
    };
    fclose(err);
    return captured;
}

static void test_json_write_failure_fails_the_run(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = response_stream,
    };

    struct hax_opts options = {.raw = 1, .json = 1};
    prompt_seen = 0;
    struct captured_run run = capture_run_broken_stdout(&provider, "hello", &options);
    EXPECT(run.result == 1);
    EXPECT(!prompt_seen); /* the stream died before the first provider call */
    EXPECT(strstr(run.err, "cannot write --json stream") != NULL);
    captured_run_free(&run);
}

static void test_plain_write_failure_fails_the_run(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = response_stream,
    };

    struct hax_opts options = {.raw = 1};
    prompt_seen = 0;
    struct captured_run run = capture_run_broken_stdout(&provider, "hello", &options);
    EXPECT(run.result == 1);
    EXPECT(prompt_seen); /* plain mode touches stdout only for the final answer */
    EXPECT(strstr(run.err, "cannot write the final answer") != NULL);
    captured_run_free(&run);
}

static void test_missing_model_is_diagnostic(void)
{
    configure_test_run();
    config_set_override("model", CONFIG_VALUE_DEFAULT);
    struct provider provider = {.name = "test-provider", .stream = response_stream};

    struct hax_opts options = {.raw = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 1);
    EXPECT_STR_EQ(run.out, "");
    EXPECT(strstr(run.err, "pass --model or set HAX_MODEL") != NULL);
    captured_run_free(&run);
}

int main(void)
{
    test_final_messages_are_pipeable();
    test_json_streams_records_and_result();
    test_json_reports_provider_error();
    test_json_write_failure_fails_the_run();
    test_plain_write_failure_fails_the_run();
    test_missing_model_is_diagnostic();
    config_free();
    T_REPORT();
}
