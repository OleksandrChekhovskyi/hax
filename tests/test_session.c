/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "provider.h"
#include "session.h"
#include "util.h"
#include "system/fs.h"
#include "system/git.h"

static void expect_write(int fd, const char *text)
{
    /* The include cleaner knows no glibc provider for ssize_t except <stdio.h>,
     * which this file has no other use for. */
    // NOLINTNEXTLINE(misc-include-cleaner)
    EXPECT(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
}

static int nullable_strings_equal(const char *a, const char *b)
{
    if (!a && !b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

static int turn_usage_equal(const struct turn_usage *a, const struct turn_usage *b)
{
    if (!a && !b)
        return 1;
    if (!a || !b)
        return 0;
    return a->usage.input_tokens == b->usage.input_tokens &&
           a->usage.output_tokens == b->usage.output_tokens &&
           a->usage.cached_tokens == b->usage.cached_tokens &&
           a->usage.cache_write_tokens == b->usage.cache_write_tokens &&
           a->usage.cache_write_1h_tokens == b->usage.cache_write_1h_tokens &&
           a->usage.cost == b->usage.cost && a->elapsed_ms == b->elapsed_ms &&
           a->cost_input == b->cost_input && a->cost_cache_read == b->cost_cache_read &&
           a->cost_cache_write == b->cost_cache_write && a->cost_output == b->cost_output &&
           a->cost_total == b->cost_total && a->cost_estimated == b->cost_estimated &&
           a->uncached_input_tokens == b->uncached_input_tokens &&
           nullable_strings_equal(a->provenance.provider_label, b->provenance.provider_label) &&
           nullable_strings_equal(a->provenance.model_label, b->provenance.model_label) &&
           nullable_strings_equal(a->provenance.effort, b->provenance.effort) &&
           nullable_strings_equal(a->provenance.served_model, b->provenance.served_model) &&
           nullable_strings_equal(a->provenance.route, b->provenance.route) &&
           nullable_strings_equal(a->provenance.response_id, b->provenance.response_id);
}

static int item_images_equal(const struct item *a, const struct item *b)
{
    if (a->n_images != b->n_images)
        return 0;
    for (size_t i = 0; i < a->n_images; i++) {
        if (!nullable_strings_equal(a->images[i].mime, b->images[i].mime) ||
            !nullable_strings_equal(a->images[i].data_b64, b->images[i].data_b64) ||
            a->images[i].width != b->images[i].width || a->images[i].height != b->images[i].height)
            return 0;
    }
    return 1;
}

static int items_equal(const struct item *a, const struct item *b)
{
    return a->kind == b->kind && nullable_strings_equal(a->text, b->text) &&
           nullable_strings_equal(a->call_id, b->call_id) &&
           nullable_strings_equal(a->tool_name, b->tool_name) &&
           nullable_strings_equal(a->tool_arguments_json, b->tool_arguments_json) &&
           nullable_strings_equal(a->output, b->output) &&
           a->output_hidden_tail == b->output_hidden_tail &&
           nullable_strings_equal(a->reasoning_json, b->reasoning_json) &&
           nullable_strings_equal(a->reasoning_text, b->reasoning_text) && a->origin == b->origin &&
           turn_usage_equal(a->usage, b->usage) && item_images_equal(a, b);
}

static void expect_item_codec_round_trip(const struct item *source)
{
    json_t *encoded = item_to_json(source);
    EXPECT(encoded != NULL);
    char *text = json_dumps(encoded, JSON_COMPACT);
    EXPECT(text != NULL);
    json_decref(encoded);

    json_t *decoded = json_loads(text, 0, NULL);
    EXPECT(decoded != NULL);
    struct item result;
    EXPECT(item_from_json(decoded, &result) == 0);
    EXPECT(items_equal(source, &result));
    EXPECT(nullable_strings_equal(source->provider, result.provider));
    EXPECT(nullable_strings_equal(source->model, result.model));
    item_free(&result);
    json_decref(decoded);
    free(text);
}

/* Static fixture objects borrow their strings and are never passed to item_free. */
static struct turn_usage ESTIMATED_USAGE = {
    .usage = {.input_tokens = 30000,
              .output_tokens = 2100,
              .cached_tokens = 16000,
              .cache_write_tokens = 8200,
              .cache_write_1h_tokens = -1,
              .cost = -1},
    .elapsed_ms = 42000,
    .uncached_input_tokens = 5800,
    .cost_input = 0.025,
    .cost_cache_read = 0.048,
    .cost_cache_write = 0.031,
    .cost_output = 0.084,
    .cost_total = 0.188,
    .cost_estimated = 1,
    .provenance = {.provider_label = (char *)"llama.cpp",
                   .model_label = (char *)"qwen3-30b-a3b",
                   .effort = (char *)"high",
                   .served_model = (char *)"deepseek/deepseek-v4",
                   .route = (char *)"Wafer",
                   .response_id = (char *)"gen-abc"},
};
static struct turn_usage EXACT_USAGE = {
    .usage = {.input_tokens = 1000,
              .output_tokens = 50,
              .cached_tokens = -1,
              .cache_write_tokens = -1,
              .cache_write_1h_tokens = -1,
              .cost = 0.0012},
    .elapsed_ms = -1,
    .uncached_input_tokens = 1000,
    .cost_input = -1,
    .cost_cache_read = -1,
    .cost_cache_write = -1,
    .cost_output = -1,
    .cost_total = 0.0012,
    .cost_estimated = 0,
};

static struct item_image IMAGES[] = {
    {.mime = (char *)"image/png", .data_b64 = (char *)"iVBORw0KGgo=", .width = 2, .height = 3},
};

static struct item CONVERSATION[] = {
    {.kind = ITEM_TURN_BOUNDARY},
    {.kind = ITEM_USER_MESSAGE, .text = (char *)"hello world"},
    {.kind = ITEM_REASONING,
     .reasoning_text = (char *)"thinking...",
     .reasoning_json = (char *)"{\"id\":\"r1\"}"},
    {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"hi there"},
    {.kind = ITEM_TOOL_CALL,
     .call_id = (char *)"c1",
     .tool_name = (char *)"bash",
     .tool_arguments_json = (char *)"{\"cmd\":\"ls\"}"},
    {.kind = ITEM_TOOL_RESULT, .call_id = (char *)"c1", .output = (char *)"file1\nfile2"},
    {.kind = ITEM_TOOL_RESULT,
     .call_id = (char *)"c2",
     .output = (char *)"Read image shot.png",
     .images = IMAGES,
     .n_images = 1},
    {.kind = ITEM_TURN_USAGE,
     .usage = &ESTIMATED_USAGE,
     .provider = (char *)"alpha",
     .model = (char *)"m1"},
    {.kind = ITEM_TURN_USAGE, .usage = &EXACT_USAGE},
    {.kind = ITEM_USER_MESSAGE,
     .text = (char *)"summary of earlier work",
     .origin = ITEM_ORIGIN_COMPACT_SEED},
    {.kind = ITEM_USER_MESSAGE, .text = (char *)"[continue]", .origin = ITEM_ORIGIN_CONTINUATION},
    {.kind = ITEM_TOOL_RESULT,
     .call_id = (char *)"c3",
     .output = (char *)"[interrupted]",
     .origin = ITEM_ORIGIN_SKIPPED},
    {.kind = ITEM_TOOL_RESULT,
     .call_id = (char *)"c4",
     .output = (char *)"error: tool calls are disabled in this session",
     .origin = ITEM_ORIGIN_REFUSED},
    {.kind = ITEM_TOOL_RESULT,
     .call_id = (char *)"c5",
     .output = (char *)"hi\n\n[finished during launch; no task created]",
     .output_hidden_tail = sizeof("\n[finished during launch; no task created]") - 1},
};
#define CONVERSATION_COUNT (sizeof(CONVERSATION) / sizeof(CONVERSATION[0]))

static void free_items(struct item *items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        item_free(&items[i]);
    free(items);
}

static void use_fresh_session_state(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
}

static char *write_session(const char *provider, const char *model, const char *effort,
                           const char *preset, const struct item *items, size_t n)
{
    struct session_log *log = session_log_open(provider, model, NULL, effort, preset);
    EXPECT(log != NULL);
    if (!log)
        return xstrdup("/nonexistent");
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, items, n);
    session_log_close(log);
    return path;
}

static void test_item_codec_round_trip(void)
{
    for (size_t i = 0; i < CONVERSATION_COUNT; i++)
        expect_item_codec_round_trip(&CONVERSATION[i]);
}

static void test_recording_control(void)
{
    use_fresh_session_state();

    /* Explicit opt-out must avoid even opening a log; "auto" is resolved by
     * the agent and remains recordable at this lower layer. */
    setenv("HAX_NO_SESSION", "1", 1);
    EXPECT(session_log_open("alpha", "m1", NULL, "high", NULL) == NULL);

    setenv("HAX_NO_SESSION", "auto", 1);
    struct session_log *log = session_log_open("alpha", "m1", NULL, "high", NULL);
    EXPECT(log != NULL);
    session_log_close(log);
    unsetenv("HAX_NO_SESSION");
}

static void test_session_round_trip(void)
{
    use_fresh_session_state();
    char *path = write_session("alpha", "m1", "high", NULL, CONVERSATION, CONVERSATION_COUNT);

    struct item *items;
    size_t n;
    struct session_meta meta;
    EXPECT(session_load(path, &items, &n, &meta) == 0);
    EXPECT(n == CONVERSATION_COUNT);
    for (size_t i = 0; i < n && i < CONVERSATION_COUNT; i++)
        EXPECT(items_equal(&items[i], &CONVERSATION[i]));
    EXPECT(meta.id != NULL && meta.id[0] != '\0');
    EXPECT(meta.cwd != NULL && meta.cwd[0] != '\0');
    EXPECT_STR_EQ(meta.provider, "alpha");
    EXPECT_STR_EQ(meta.model, "m1");
    EXPECT_STR_EQ(meta.effort, "high");

    free_items(items, n);
    session_meta_free(&meta);
    free(path);
}

static void test_reasoning_provenance_round_trip(void)
{
    use_fresh_session_state();
    struct item conversation[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"q"},
        {.kind = ITEM_REASONING,
         .reasoning_json = (char *)"{\"id\":\"enc\"}",
         .provider = (char *)"pa",
         .model = (char *)"mX"},
        {.kind = ITEM_REASONING, .reasoning_text = (char *)"plain cot"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"a"},
    };
    char *path = write_session("pa", "ma", NULL, NULL, conversation, 4);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    const struct item *encoded = NULL;
    const struct item *text = NULL;
    for (size_t i = 0; i < n; i++) {
        if (items[i].kind != ITEM_REASONING)
            continue;
        if (items[i].reasoning_json)
            encoded = &items[i];
        else
            text = &items[i];
    }
    EXPECT(encoded && encoded->reasoning_json && encoded->provider &&
           strcmp(encoded->provider, "pa") == 0);
    EXPECT(encoded && encoded->model && strcmp(encoded->model, "mX") == 0);
    EXPECT(text && text->reasoning_text && strcmp(text->reasoning_text, "plain cot") == 0);
    EXPECT(text && text->provider && strcmp(text->provider, "pa") == 0);
    EXPECT(text && text->model && strcmp(text->model, "ma") == 0);

    free_items(items, n);
    free(path);
}

static void test_session_listing(void)
{
    use_fresh_session_state();
    char *path = write_session("alpha", "m1", "high", NULL, CONVERSATION, CONVERSATION_COUNT);
    struct item *items;
    size_t n;
    struct session_meta meta;
    EXPECT(session_load(path, &items, &n, &meta) == 0);
    char *saved_id = xstrdup(meta.id);
    free_items(items, n);
    session_meta_free(&meta);

    char cwd[4096];
    EXPECT(getcwd(cwd, sizeof(cwd)) != NULL);
    struct session_entry *list;
    size_t list_n;
    EXPECT(session_list(cwd, &list, &list_n) == 0);
    EXPECT(list_n == 1);
    const struct session_entry *found = NULL;
    for (size_t i = 0; i < list_n; i++) {
        if (strcmp(list[i].path, path) == 0)
            found = &list[i];
    }
    EXPECT(found != NULL);
    if (found) {
        EXPECT_STR_EQ(found->id, saved_id);
        EXPECT(found->label.prompt == NULL);
        struct session_label label;
        session_label_read(found->path, 64, &label);
        EXPECT(label.prompt != NULL && strstr(label.prompt, "hello world") != NULL);
        EXPECT_STR_EQ(label.model, "m1");
        session_label_free(&label);
    }

    session_list_free(list, list_n);
    free(saved_id);
    free(path);
}

static void test_session_file_permissions(void)
{
    use_fresh_session_state();
    char *path = write_session("alpha", "m1", NULL, NULL, CONVERSATION, CONVERSATION_COUNT);

    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 0077) == 0);

    free(path);
}

static void test_resume_appends_only_new_items(void)
{
    use_fresh_session_state();
    char *path = write_session("alpha", "m1", "high", NULL, CONVERSATION, CONVERSATION_COUNT);
    struct item extended[CONVERSATION_COUNT + 2];
    memcpy(extended, CONVERSATION, sizeof(CONVERSATION));
    extended[CONVERSATION_COUNT] = (struct item){.kind = ITEM_TURN_BOUNDARY};
    extended[CONVERSATION_COUNT + 1] =
        (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)"again"};

    struct session_log *log =
        session_log_resume(path, "alpha", "m1", "high", NULL, CONVERSATION_COUNT);
    EXPECT(log != NULL);
    session_log_append(log, extended, CONVERSATION_COUNT + 2);
    session_log_close(log);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == CONVERSATION_COUNT + 2);
    if (n == CONVERSATION_COUNT + 2) {
        EXPECT(items[n - 1].kind == ITEM_USER_MESSAGE);
        EXPECT_STR_EQ(items[n - 1].text, "again");
    }

    free_items(items, n);
    free(path);
}

static void test_prompt_labels(void)
{
    use_fresh_session_state();
    struct item boundary[] = {{.kind = ITEM_TURN_BOUNDARY}};
    char *empty_path = write_session("pa", "ma", NULL, NULL, boundary, 1);
    struct session_label label;
    session_label_read(empty_path, 64, &label);
    EXPECT(label.prompt == NULL);
    session_label_free(&label);
    free(empty_path);

    struct item compacted[] = {
        {.kind = ITEM_USER_MESSAGE,
         .text = (char *)"condensed summary",
         .origin = ITEM_ORIGIN_COMPACT_SEED},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"continuing"},
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"real question"},
    };

    char *continued_path = write_session("pa", "ma", NULL, NULL, compacted, 4);
    session_label_read(continued_path, 64, &label);
    EXPECT(label.prompt != NULL && strstr(label.prompt, "real question") != NULL);
    EXPECT(label.prompt == NULL || strstr(label.prompt, "condensed summary") == NULL);
    session_label_free(&label);
    free(continued_path);

    char *seed_path = write_session("pa", "ma", NULL, NULL, compacted, 2);
    session_label_read(seed_path, 64, &label);
    EXPECT(label.prompt != NULL);
    if (label.prompt)
        EXPECT_STR_EQ(label.prompt, "(compacted)");
    session_label_free(&label);
    free(seed_path);
}

static void test_label_reports_recorded_context(void)
{
    use_fresh_session_state();
    char *path = write_session("alpha", "m1", NULL, "review", CONVERSATION, CONVERSATION_COUNT);

    struct session_label label;
    session_label_read(path, 64, &label);
    EXPECT_STR_EQ(label.provider, "alpha");
    EXPECT_STR_EQ(label.model, "m1");
    EXPECT_STR_EQ(label.preset, "review");

    /* Comparing against a live probe ties the header writer to the label reader without assuming
     * the build tree is a repository. */
    struct git_state git;
    git_state_probe(&git);
    if (git.subject)
        EXPECT_STR_EQ(label.git_subject, git.subject);
    else
        EXPECT(label.git_subject == NULL);
    if (git.branch)
        EXPECT_STR_EQ(label.git_branch, git.branch);
    else
        EXPECT(label.git_branch == NULL);
    git_state_free(&git);

    session_label_free(&label);
    free(path);
}

/* A local weights path identifies nothing in a picker row, so the provider's rendering of it is
 * what gets recorded and read back. */
static void test_label_prefers_recorded_model_label(void)
{
    use_fresh_session_state();
    struct session_log *log =
        session_log_open("llama.cpp", "/models/Qwen3.6-35B.gguf", "Qwen3.6-35B", NULL, NULL);
    EXPECT(log != NULL);
    if (!log)
        return;
    char *path = xstrdup(session_log_path(log));
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = (char *)"hello"}};
    session_log_append(log, items, 1);
    session_log_close(log);

    struct session_label label;
    session_label_read(path, 64, &label);
    EXPECT_STR_EQ(label.model, "Qwen3.6-35B");
    session_label_free(&label);
    free(path);
}

static void test_resume_repairs_torn_final_line(void)
{
    char path[] = "/tmp/hax_torn_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd < 0)
        return;

    const char *torn = "{\"type\":\"session\",\"version\":1,\"provider\":\"pa\",\"model\":\"ma\"}\n"
                       "{\"kind\":\"turn_boundary\"}\n"
                       "{\"kind\":\"user\",\"text\":\"hi\"}\n"
                       "{\"kind\":\"assistant\",\"text\":\"hello\"}\n"
                       "{\"kind\":\"user\",\"text\":\"torn";
    expect_write(fd, torn);
    close(fd);

    struct item *base;
    size_t base_n;
    EXPECT(session_load(path, &base, &base_n, NULL) == 0);
    EXPECT(base_n == 3);

    struct item extended[5];
    memcpy(extended, base, base_n * sizeof(struct item));
    extended[base_n] = (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)"after crash"};
    struct session_log *log = session_log_resume(path, "pa", "ma", NULL, NULL, base_n);
    EXPECT(log != NULL);
    session_log_append(log, extended, base_n + 1);
    session_log_close(log);
    free_items(base, base_n);

    struct item *after;
    size_t after_n;
    EXPECT(session_load(path, &after, &after_n, NULL) == 0);
    EXPECT(after_n == 4);
    if (after_n == 4)
        EXPECT_STR_EQ(after[3].text, "after crash");
    free_items(after, after_n);
    unlink(path);
}

static void test_load_trims_dangling_tool_call(void)
{
    use_fresh_session_state();
    struct item conversation[] = {
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"run it"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"sure"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = (char *)"c1",
         .tool_name = (char *)"bash",
         .tool_arguments_json = (char *)"{}"},
    };
    char *path = write_session("pa", "ma", NULL, NULL, conversation, 4);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == 3);
    for (size_t i = 0; i < n; i++)
        EXPECT(items[i].kind != ITEM_TOOL_CALL);

    free_items(items, n);
    free(path);
}

static void test_log_materialization(void)
{
    use_fresh_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL, NULL);
    EXPECT(log != NULL);

    EXPECT(session_log_materialized(log) == 0);
    struct item turn[] = {{.kind = ITEM_TURN_BOUNDARY},
                          {.kind = ITEM_USER_MESSAGE, .text = (char *)"hi"}};
    session_log_append(log, turn, 2);
    EXPECT(session_log_materialized(log) != 0);
    session_log_close(log);
    EXPECT(session_log_materialized(NULL) == 0);
}

static struct item UNDO_CONVERSATION[] = {
    {.kind = ITEM_TURN_BOUNDARY},
    {.kind = ITEM_USER_MESSAGE, .text = (char *)"t0"},
    {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"a0"},
    {.kind = ITEM_TURN_BOUNDARY},
    {.kind = ITEM_USER_MESSAGE, .text = (char *)"t1"},
    {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"a1"},
    {.kind = ITEM_TURN_BOUNDARY},
    {.kind = ITEM_USER_MESSAGE, .text = (char *)"t2"},
    {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"a2"},
};

static void test_truncate_and_reappend(void)
{
    use_fresh_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL, NULL);
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, UNDO_CONVERSATION, 9);

    EXPECT(session_log_truncate(log, 2, 6) == 0);
    struct item replacement[7];
    memcpy(replacement, UNDO_CONVERSATION, 6 * sizeof(struct item));
    replacement[6] = (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)"redo"};
    session_log_append(log, replacement, 7);
    session_log_close(log);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == 7);
    if (n == 7) {
        EXPECT_STR_EQ(items[5].text, "a1");
        EXPECT_STR_EQ(items[6].text, "redo");
    }
    for (size_t i = 0; i < n; i++)
        EXPECT(!(items[i].text && strcmp(items[i].text, "t2") == 0));

    free_items(items, n);
    free(path);
}

static void test_truncate_all_turns(void)
{
    use_fresh_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL, NULL);
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, UNDO_CONVERSATION, 9);

    EXPECT(session_log_truncate(log, 0, 0) == 0);
    session_log_close(log);
    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == 0);

    free_items(items, n);
    free(path);
}

static void test_fork_copies_prefix_without_touching_source(void)
{
    use_fresh_session_state();
    char *source_path = write_session("pa", "ma", "hi", NULL, UNDO_CONVERSATION, 9);
    struct item *items;
    size_t n;
    struct session_meta meta;
    EXPECT(session_load(source_path, &items, &n, &meta) == 0);
    char *source_id = xstrdup(meta.id);
    free_items(items, n);
    session_meta_free(&meta);

    char *fork_path = NULL;
    EXPECT(session_fork_file(source_path, 1, &fork_path) == 0);
    EXPECT(fork_path != NULL);
    if (fork_path) {
        EXPECT(session_load(fork_path, &items, &n, &meta) == 0);
        EXPECT(n == 3);
        if (n == 3)
            EXPECT_STR_EQ(items[1].text, "t0");
        for (size_t i = 0; i < n; i++)
            EXPECT(!(items[i].text && strcmp(items[i].text, "t1") == 0));
        EXPECT(meta.id != NULL && strcmp(meta.id, source_id) != 0);
        EXPECT_STR_EQ(meta.provider, "pa");
        EXPECT_STR_EQ(meta.model, "ma");
        EXPECT_STR_EQ(meta.effort, "hi");
        free_items(items, n);
        session_meta_free(&meta);

        size_t data_n;
        char *data = slurp_file(fork_path, &data_n);
        EXPECT(data != NULL);
        if (data) {
            EXPECT(strstr(data, "forked_from") != NULL);
            EXPECT(strstr(data, source_id) != NULL);
            free(data);
        }
        free(fork_path);
    }

    EXPECT(session_load(source_path, &items, &n, NULL) == 0);
    EXPECT(n == 9);
    free_items(items, n);

    char *clone_path = NULL;
    EXPECT(session_fork_file(source_path, 3, &clone_path) == 0);
    if (clone_path) {
        EXPECT(session_load(clone_path, &items, &n, NULL) == 0);
        EXPECT(n == 9);
        free_items(items, n);
        free(clone_path);
    }

    free(source_id);
    free(source_path);
}

static void test_selection_metadata_tracks_productive_switches(void)
{
    use_fresh_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, "hi", "review");
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));

    session_log_set_meta(log, "pb", "mb", NULL, NULL, NULL);
    session_log_append(log, UNDO_CONVERSATION, 3);
    struct session_meta meta;
    EXPECT(session_read_meta(path, &meta) == 0);
    EXPECT_STR_EQ(meta.provider, "pb");
    EXPECT_STR_EQ(meta.model, "mb");
    EXPECT(meta.effort == NULL);
    EXPECT(meta.preset == NULL);
    session_meta_free(&meta);

    session_log_set_meta(log, "pc", "mc", NULL, "low", "review");
    session_log_append(log, UNDO_CONVERSATION, 6);
    session_log_close(log);
    EXPECT(session_read_meta(path, &meta) == 0);
    EXPECT_STR_EQ(meta.provider, "pc");
    EXPECT_STR_EQ(meta.model, "mc");
    EXPECT_STR_EQ(meta.effort, "low");
    EXPECT_STR_EQ(meta.preset, "review");
    EXPECT(meta.id != NULL && meta.cwd != NULL);
    session_meta_free(&meta);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, &meta) == 0);
    EXPECT(n == 6);
    EXPECT_STR_EQ(meta.provider, "pc");
    EXPECT_STR_EQ(meta.preset, "review");
    free_items(items, n);
    session_meta_free(&meta);

    struct session_log *resumed = session_log_resume(path, "pc", "mc", "low", "review", 6);
    EXPECT(resumed != NULL);
    session_log_set_meta(resumed, "pc", "md", NULL, NULL, NULL);
    EXPECT(session_read_meta(path, &meta) == 0);
    EXPECT_STR_EQ(meta.model, "mc");
    EXPECT_STR_EQ(meta.preset, "review");
    session_meta_free(&meta);

    session_log_append(resumed, UNDO_CONVERSATION, 9);
    session_log_close(resumed);
    EXPECT(session_read_meta(path, &meta) == 0);
    EXPECT_STR_EQ(meta.model, "md");
    EXPECT(meta.preset == NULL);
    EXPECT(meta.effort == NULL);
    session_meta_free(&meta);
    free(path);
}

static void test_discarded_selection_stays_out_of_log(void)
{
    use_fresh_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL, NULL);
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, UNDO_CONVERSATION, 3);

    /* A selection staged for the next conversation (a `/new <preset>`) must not ride a
     * synthetic conversation-ending append into this record. */
    session_log_set_meta(log, "pb", "mb", NULL, NULL, "next");
    session_log_discard_selection(log);
    session_log_append(log, UNDO_CONVERSATION, 6);
    session_log_close(log);

    struct session_meta meta;
    EXPECT(session_read_meta(path, &meta) == 0);
    EXPECT_STR_EQ(meta.provider, "pa");
    EXPECT_STR_EQ(meta.model, "ma");
    EXPECT(meta.preset == NULL);
    session_meta_free(&meta);
    free(path);
}

static void test_truncate_restates_live_selection(void)
{
    use_fresh_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL, NULL);
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, UNDO_CONVERSATION, 6);
    session_log_set_meta(log, "pb", "mb", NULL, NULL, "stance");
    session_log_append(log, UNDO_CONVERSATION, 9);

    EXPECT(session_log_truncate(log, 1, 3) == 0);
    session_log_append(log, UNDO_CONVERSATION, 6);
    session_log_close(log);

    struct session_meta meta;
    EXPECT(session_read_meta(path, &meta) == 0);
    EXPECT_STR_EQ(meta.provider, "pb");
    EXPECT_STR_EQ(meta.model, "mb");
    EXPECT_STR_EQ(meta.preset, "stance");
    session_meta_free(&meta);
    free(path);
}

static void test_read_meta_failure_initializes_output(void)
{
    struct session_meta meta;

    EXPECT(session_read_meta("/nonexistent/hax-session.jsonl", &meta) == -1);
    EXPECT(meta.provider == NULL && meta.id == NULL);
}

static void test_session_readers_reject_fifo(void)
{
    char *path = xasprintf("%s/session.jsonl", t_tempdir());
    EXPECT(mkfifo(path, 0600) == 0);

    struct session_meta meta;
    EXPECT(session_read_meta(path, &meta) == -1);
    EXPECT(meta.provider == NULL && meta.id == NULL);

    struct item *items = (struct item *)1;
    size_t count = 1;
    EXPECT(session_load(path, &items, &count, NULL) == -1);
    EXPECT(items == NULL);
    EXPECT(count == 0);

    unlink(path);
    free(path);
}

static void test_load_enforces_image_count_cap(void)
{
    char path[] = "/tmp/hax_imgcap_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd < 0)
        return;

    const char *header =
        "{\"type\":\"session\",\"version\":1,\"provider\":\"pa\",\"model\":\"ma\"}\n";
    expect_write(fd, header);
    for (int i = 0; i < IMAGE_REQUEST_MAX_COUNT + 1; i++) {
        char *line = xasprintf("{\"kind\":\"tool_result\",\"call_id\":\"c%d\",\"output\":\"r\","
                               "\"images\":[{\"mime\":\"image/png\",\"data\":\"QUJD\","
                               "\"width\":2,\"height\":1}]}\n",
                               i);
        expect_write(fd, line);
        free(line);
    }
    close(fd);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    size_t total = 0;
    size_t degraded = 0;
    for (size_t i = 0; i < n; i++) {
        total += items[i].n_images;
        if (items[i].n_images == 0 && items[i].output && strstr(items[i].output, "[image"))
            degraded++;
    }
    EXPECT(total == IMAGE_REQUEST_MAX_COUNT);
    EXPECT(degraded == 1);

    free_items(items, n);
    unlink(path);
}

/* A summarized prefix is never sent, so its images must not spend the window's image budget. */
static void test_load_budgets_images_from_compaction_seed(void)
{
    char path[] = "/tmp/hax_imgfloor_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd < 0)
        return;

    const char *header =
        "{\"type\":\"session\",\"version\":1,\"provider\":\"pa\",\"model\":\"ma\"}\n";
    expect_write(fd, header);
    for (int i = 0; i < IMAGE_REQUEST_MAX_COUNT; i++) {
        char *line = xasprintf("{\"kind\":\"tool_result\",\"call_id\":\"c%d\",\"output\":\"r\","
                               "\"images\":[{\"mime\":\"image/png\",\"data\":\"QUJD\","
                               "\"width\":2,\"height\":1}]}\n",
                               i);
        expect_write(fd, line);
        free(line);
    }
    const char *after_seed =
        "{\"kind\":\"user\",\"text\":\"summary\",\"origin\":\"compact_seed\"}\n"
        "{\"kind\":\"tool_result\",\"call_id\":\"live\",\"output\":\"r\","
        "\"images\":[{\"mime\":\"image/png\",\"data\":\"QUJD\",\"width\":2,\"height\":1}]}\n";
    expect_write(fd, after_seed);
    close(fd);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == IMAGE_REQUEST_MAX_COUNT + 2);
    EXPECT(items[n - 1].n_images == 1);

    free_items(items, n);
    unlink(path);
}

int main(void)
{
    test_item_codec_round_trip();
    test_recording_control();
    test_session_round_trip();
    test_reasoning_provenance_round_trip();
    test_session_listing();
    test_session_file_permissions();
    test_resume_appends_only_new_items();
    test_prompt_labels();
    test_label_reports_recorded_context();
    test_label_prefers_recorded_model_label();
    test_resume_repairs_torn_final_line();
    test_load_trims_dangling_tool_call();
    test_log_materialization();
    test_truncate_and_reappend();
    test_truncate_all_turns();
    test_fork_copies_prefix_without_touching_source();
    test_selection_metadata_tracks_productive_switches();
    test_discarded_selection_stays_out_of_log();
    test_truncate_restates_live_selection();
    test_read_meta_failure_initializes_output();
    test_session_readers_reject_fifo();
    test_load_enforces_image_count_cap();
    test_load_budgets_images_from_compaction_seed();
    T_REPORT();
}
