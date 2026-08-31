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
#include "session.h"
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

/* Read end of the piped stdout below; a stream closes it to mimic a mid-run consumer death. */
static int consumer_read_fd = -1;

/* Run with stdout as a pipe, under the default SIGPIPE disposition: the run itself must
 * neutralize SIGPIPE so a write to a dead pipe surfaces as a checked error instead of killing
 * the process before task cleanup. With `reader_exited` the pipe is broken from the start;
 * otherwise the read end stays open in consumer_read_fd for the provider stream to close. */
static struct captured_run capture_run_piped_stdout(struct provider *provider, const char *prompt,
                                                    const struct hax_opts *options,
                                                    int reader_exited)
{
    void (*saved_sigpipe)(int) = signal(SIGPIPE, SIG_DFL);
    int pipe_fds[2];
    EXPECT(pipe(pipe_fds) == 0);
    if (reader_exited)
        close(pipe_fds[0]);
    else
        consumer_read_fd = pipe_fds[0];

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
    if (consumer_read_fd >= 0) {
        close(consumer_read_fd);
        consumer_read_fd = -1;
    }
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
    struct captured_run run = capture_run_piped_stdout(&provider, "hello", &options, 1);
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
    struct captured_run run = capture_run_piped_stdout(&provider, "hello", &options, 1);
    EXPECT(run.result == 1);
    EXPECT(prompt_seen); /* plain mode touches stdout only for the final answer */
    EXPECT(strstr(run.err, "cannot write the final answer") != NULL);
    captured_run_free(&run);
}

/* The run's own SIGINT handler must latch an abort that reaches the provider via the tick,
 * standing in for a driver signalling mid-stream. */
static int interrupted_stream(struct provider *provider, const struct context *context,
                              const char *model, stream_cb callback, void *user, http_tick_cb tick,
                              void *tick_user)
{
    (void)provider;
    (void)context;
    (void)model;

    struct stream_event delta = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "partial"}};
    callback(&delta, user);
    raise(SIGINT);
    EXPECT(tick != NULL);
    EXPECT(tick(tick_user));
    return -1;
}

static void test_sigint_interrupts_gracefully(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = interrupted_stream,
    };

    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 130);
    EXPECT(strstr(run.err, "interrupted") != NULL);
    EXPECT(strstr(run.out, "\"outcome\": \"interrupted\"") != NULL ||
           strstr(run.out, "\"outcome\":\"interrupted\"") != NULL);
    /* Abort repair kept the partial text and marked it. */
    EXPECT(strstr(run.out, "[interrupted]") != NULL);
    captured_run_free(&run);
}

/* The same terminal Ctrl-C that latches the abort also kills the pipeline's consumer. */
static int interrupted_stream_dead_consumer(struct provider *provider,
                                            const struct context *context, const char *model,
                                            stream_cb callback, void *user, http_tick_cb tick,
                                            void *tick_user)
{
    (void)provider;
    (void)context;
    (void)model;

    struct stream_event delta = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "partial"}};
    callback(&delta, user);
    close(consumer_read_fd);
    consumer_read_fd = -1;
    raise(SIGINT);
    EXPECT(tick != NULL);
    EXPECT(tick(tick_user));
    return -1;
}

/* Losing the stream to the user's own Ctrl-C is expected collateral, not a second failure:
 * the run keeps the interrupt's single diagnostic and its 130 exit status. */
static void test_sigint_with_dead_consumer_keeps_interrupt_outcome(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = interrupted_stream_dead_consumer,
    };

    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run_piped_stdout(&provider, "hello", &options, 0);
    EXPECT(run.result == 130);
    EXPECT(strstr(run.err, "interrupted") != NULL);
    EXPECT(strstr(run.err, "cannot write --json stream") == NULL);
    captured_run_free(&run);
}

static int paused_stream(struct provider *provider, const struct context *context,
                         const char *model, stream_cb callback, void *user, http_tick_cb tick,
                         void *tick_user)
{
    (void)provider;
    (void)context;
    (void)model;
    (void)tick;
    (void)tick_user;

    raise(SIGUSR1);
    struct stream_event events[] = {
        {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "working"}},
        {.kind = EV_TOOL_CALL_START, .u.tool_call_start = {.id = "call-1", .name = "bash"}},
        {.kind = EV_TOOL_CALL_DELTA, .u.tool_call_delta = {.id = "call-1", .args_delta = "{}"}},
        {.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = "call-1"}},
        {.kind = EV_DONE,
         .u.done = {.stop_reason = "tool_use",
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

/* A pause lets the streamed turn and its tool batch finish, then stops at the seam instead of
 * launching the follow-up turn. */
static void test_sigusr1_pauses_at_seam(void)
{
    configure_test_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = paused_stream,
    };

    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 1);
    EXPECT(strstr(run.err, "paused") != NULL);
    EXPECT(strstr(run.out, "\"outcome\": \"paused\"") != NULL ||
           strstr(run.out, "\"outcome\":\"paused\"") != NULL);
    /* The batch was paired before the stop; the seam is clean. */
    EXPECT(strstr(run.out, "tool_result") != NULL);
    captured_run_free(&run);

    /* Outside the run's graceful window the pause signal must be ignored, not fatal. */
    raise(SIGUSR1);
}

/* Recorded-session variant of configure_test_run: each call isolates its session files. */
static void configure_recorded_run(void)
{
    configure_test_run();
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    config_set_override("no_session", "0");
}

/* Path of the only session the current test recorded. */
static char *recorded_session_path(void)
{
    char *cwd = getcwd(NULL, 0);
    struct session_entry *sessions = NULL;
    size_t count = 0;
    session_list(cwd, &sessions, &count);
    EXPECT(count == 1);
    char *path = count > 0 ? xstrdup(sessions[0].path) : NULL;
    session_list_free(sessions, count);
    free(cwd);
    return path;
}

/* A pause leaves a clean seam, so the promptless resume continues it verbatim: no synthetic
 * user turn appears in the resumed run's records. */
static void test_promptless_resume_continues_clean_seam(void)
{
    configure_recorded_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = paused_stream,
    };
    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 1);
    captured_run_free(&run);

    char *path = recorded_session_path();
    struct hax_opts resume_options = {.raw = 1, .json = 1, .resume_path = path};
    provider.stream = response_stream;
    run = capture_run(&provider, NULL, &resume_options);
    EXPECT(run.result == 0);
    EXPECT(strstr(run.out, "\"outcome\":\"complete\"") != NULL);
    EXPECT(strstr(run.out, "\"kind\":\"user\"") == NULL);
    captured_run_free(&run);
    free(path);
}

/* An interrupt leaves a marked tail, so the promptless resume must speak for the user. */
static void test_promptless_resume_adds_continuation(void)
{
    configure_recorded_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = interrupted_stream,
    };
    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 130);
    captured_run_free(&run);

    char *path = recorded_session_path();
    struct hax_opts resume_options = {.raw = 1, .json = 1, .resume_path = path};
    provider.stream = response_stream;
    run = capture_run(&provider, NULL, &resume_options);
    EXPECT(run.result == 0);
    EXPECT(strstr(run.out, "\"origin\":\"continuation\"") != NULL);
    EXPECT(strstr(run.out, "\"outcome\":\"complete\"") != NULL);
    captured_run_free(&run);
    free(path);
}

static int over_threshold_stream(struct provider *provider, const struct context *context,
                                 const char *model, stream_cb callback, void *user,
                                 http_tick_cb tick, void *tick_user)
{
    (void)provider;
    (void)context;
    (void)model;
    (void)tick;
    (void)tick_user;

    struct stream_event delta = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "done"}};
    if (callback(&delta, user))
        return -1;
    struct stream_event done = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = "end_turn",
                   .usage = {.input_tokens = 900,
                             .output_tokens = 50,
                             .cached_tokens = -1,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = -1}},
    };
    return callback(&done, user);
}

static int summarizing_calls;

static int summarizing_stream(struct provider *provider, const struct context *context,
                              const char *model, stream_cb callback, void *user, http_tick_cb tick,
                              void *tick_user)
{
    (void)provider;
    (void)context;
    (void)model;
    (void)tick;
    (void)tick_user;

    summarizing_calls++;
    const char *text = summarizing_calls == 1 ? "The summary." : "The answer.";
    struct stream_event delta = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = text}};
    if (callback(&delta, user))
        return -1;
    struct stream_event done = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = "end_turn",
                   .usage = {.input_tokens = -1,
                             .output_tokens = -1,
                             .cached_tokens = -1,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = -1}},
    };
    return callback(&done, user);
}

/* A resumed run whose recorded window crosses the threshold compacts before the first continued
 * request: the first provider call is the summarization, the second the continued turn. */
static void test_resumed_run_compacts_oversized_window(void)
{
    configure_recorded_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = over_threshold_stream,
    };
    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 0);
    captured_run_free(&run);

    char *path = recorded_session_path();
    config_set_override("context_limit", "1000"); /* usage 950 crosses the default 85% */
    struct hax_opts resume_options = {.raw = 1, .json = 1, .resume_path = path};
    provider.stream = summarizing_stream;
    summarizing_calls = 0;
    run = capture_run(&provider, NULL, &resume_options);
    EXPECT(run.result == 0);
    EXPECT(summarizing_calls == 2);
    EXPECT(strstr(run.out, "\"origin\":\"compact_seed\"") != NULL);
    EXPECT(strstr(run.out, "\"text\":\"The answer.\"") != NULL);
    captured_run_free(&run);
    free(path);
    config_set_override("context_limit", CONFIG_VALUE_DEFAULT);
}

static int aborted_compaction_calls;

static int aborted_compaction_stream(struct provider *provider, const struct context *context,
                                     const char *model, stream_cb callback, void *user,
                                     http_tick_cb tick, void *tick_user)
{
    (void)provider;
    (void)context;
    (void)model;
    (void)callback;
    (void)user;

    aborted_compaction_calls++;
    raise(SIGINT);
    EXPECT(tick != NULL);
    EXPECT(tick(tick_user));
    return -1;
}

/* A signal during the resumed compaction cancels the whole send: no seed, no turn launched
 * against the uncompacted window, and the run still reports the interrupt. */
static void test_sigint_during_resumed_compaction(void)
{
    configure_recorded_run();
    struct provider provider = {
        .name = "test-provider",
        .default_model = "unused",
        .stream = over_threshold_stream,
    };
    struct hax_opts options = {.raw = 1, .json = 1};
    struct captured_run run = capture_run(&provider, "hello", &options);
    EXPECT(run.result == 0);
    captured_run_free(&run);

    char *path = recorded_session_path();
    config_set_override("context_limit", "1000");
    struct hax_opts resume_options = {.raw = 1, .json = 1, .resume_path = path};
    provider.stream = aborted_compaction_stream;
    aborted_compaction_calls = 0;
    run = capture_run(&provider, NULL, &resume_options);
    EXPECT(run.result == 130);
    EXPECT(aborted_compaction_calls == 1);
    EXPECT(strstr(run.out, "\"outcome\":\"interrupted\"") != NULL);
    EXPECT(strstr(run.out, "compact_seed") == NULL);
    EXPECT(strstr(run.err, "interrupted") != NULL);
    captured_run_free(&run);
    free(path);
    config_set_override("context_limit", CONFIG_VALUE_DEFAULT);
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
    test_sigint_interrupts_gracefully();
    test_sigint_with_dead_consumer_keeps_interrupt_outcome();
    test_sigusr1_pauses_at_seam();
    test_promptless_resume_continues_clean_seam();
    test_promptless_resume_adds_continuation();
    test_resumed_run_compacts_oversized_window();
    test_sigint_during_resumed_compaction();
    test_missing_model_is_diagnostic();
    config_free();
    T_REPORT();
}
