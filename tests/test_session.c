/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness.h"
#include "session.h"
#include "util.h"

static int streq0(const char *a, const char *b)
{
    if (!a && !b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

static int usage_eq(const struct turn_usage *a, const struct turn_usage *b)
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
           a->cost_in == b->cost_in && a->cost_cache_read == b->cost_cache_read &&
           a->cost_cache_write == b->cost_cache_write && a->cost_out == b->cost_out &&
           a->cost_total == b->cost_total && a->cost_estimated == b->cost_estimated &&
           a->in_tokens == b->in_tokens;
}

static int images_eq(const struct item *a, const struct item *b)
{
    if (a->n_images != b->n_images)
        return 0;
    for (size_t i = 0; i < a->n_images; i++) {
        if (!streq0(a->images[i].mime, b->images[i].mime) ||
            !streq0(a->images[i].data_b64, b->images[i].data_b64) ||
            a->images[i].width != b->images[i].width || a->images[i].height != b->images[i].height)
            return 0;
    }
    return 1;
}

static int item_eq(const struct item *a, const struct item *b)
{
    return a->kind == b->kind && streq0(a->text, b->text) && streq0(a->call_id, b->call_id) &&
           streq0(a->tool_name, b->tool_name) &&
           streq0(a->tool_arguments_json, b->tool_arguments_json) && streq0(a->output, b->output) &&
           streq0(a->reasoning_json, b->reasoning_json) &&
           streq0(a->reasoning_text, b->reasoning_text) && a->compact_seed == b->compact_seed &&
           usage_eq(a->usage, b->usage) && images_eq(a, b);
}

/* Round-trip one item through item_to_json -> json text -> json -> item,
 * exercising the serialization layer the session file relies on. */
static void check_codec(const struct item *src)
{
    json_t *o = item_to_json(src);
    EXPECT(o != NULL);
    char *s = json_dumps(o, JSON_COMPACT);
    EXPECT(s != NULL);
    json_decref(o);

    json_t *back = json_loads(s, 0, NULL);
    EXPECT(back != NULL);
    struct item got;
    EXPECT(item_from_json(back, &got) == 0);
    EXPECT(item_eq(src, &got));
    EXPECT(streq0(src->provider, got.provider));
    EXPECT(streq0(src->model, got.model));
    item_free(&got);
    json_decref(back);
    free(s);
}

/* An estimated-cost usage footer (unreported fields as -1 sentinels) and
 * an exact one (reported charge, no decomposition) — static like the
 * strings below, so the fixture items own nothing. */
static struct turn_usage TU_EST = {
    .usage = {.input_tokens = 30000,
              .output_tokens = 2100,
              .cached_tokens = 16000,
              .cache_write_tokens = 8200,
              .cache_write_1h_tokens = -1,
              .cost = -1},
    .elapsed_ms = 42000,
    .in_tokens = 5800, /* 30000 - 16000 cached - 8200 written */
    .cost_in = 0.025,
    .cost_cache_read = 0.048,
    .cost_cache_write = 0.031,
    .cost_out = 0.084,
    .cost_total = 0.188,
    .cost_estimated = 1,
};
static struct turn_usage TU_EXACT = {
    .usage = {.input_tokens = 1000,
              .output_tokens = 50,
              .cached_tokens = -1,
              .cache_write_tokens = -1,
              .cache_write_1h_tokens = -1,
              .cost = 0.0012},
    .elapsed_ms = -1,
    .in_tokens = 1000,
    .cost_in = -1,
    .cost_cache_read = -1,
    .cost_cache_write = -1,
    .cost_out = -1,
    .cost_total = 0.0012,
    .cost_estimated = 0,
};

static struct item_image IMAGES[] = {
    {.mime = (char *)"image/png", .data_b64 = (char *)"iVBORw0KGgo=", .width = 2, .height = 3},
};

/* A representative conversation covering every item kind. Strings are
 * literals — never freed, never written — so the array lives on the
 * stack with no ownership bookkeeping. */
static struct item CONVO[] = {
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
    {.kind = ITEM_TURN_USAGE, .usage = &TU_EST, .provider = (char *)"alpha", .model = (char *)"m1"},
    {.kind = ITEM_TURN_USAGE, .usage = &TU_EXACT},
    {.kind = ITEM_USER_MESSAGE, .text = (char *)"summary of earlier work", .compact_seed = 1},
};
#define CONVO_N (sizeof(CONVO) / sizeof(CONVO[0]))

static void free_items(struct item *items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        item_free(&items[i]);
    free(items);
}

static void reset_session_state(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
}

static char *write_session(const char *provider, const char *model, const char *effort,
                           const char *preset, const struct item *items, size_t n)
{
    struct session_log *log = session_log_open(provider, model, effort, preset);
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
    /* Session persistence depends on every item kind surviving the JSON codec
     * without losing provider-facing state. */
    for (size_t i = 0; i < CONVO_N; i++)
        check_codec(&CONVO[i]);
}

static void test_recording_control(void)
{
    reset_session_state();

    /* Explicit opt-out must avoid even opening a log; "auto" is resolved by
     * the agent and remains recordable at this lower layer. */
    setenv("HAX_NO_SESSION", "1", 1);
    EXPECT(session_log_open("alpha", "m1", "high", NULL) == NULL);

    setenv("HAX_NO_SESSION", "auto", 1);
    struct session_log *log = session_log_open("alpha", "m1", "high", NULL);
    EXPECT(log != NULL);
    session_log_close(log);
    unsetenv("HAX_NO_SESSION");
}

static void test_session_round_trip(void)
{
    reset_session_state();
    char *path = write_session("alpha", "m1", "high", NULL, CONVO, CONVO_N);

    /* A resume must recover both the complete conversation and the selection
     * needed to continue it with the same provider. */
    struct item *items;
    size_t n;
    struct session_meta meta;
    EXPECT(session_load(path, &items, &n, &meta) == 0);
    EXPECT(n == CONVO_N);
    for (size_t i = 0; i < n && i < CONVO_N; i++)
        EXPECT(item_eq(&items[i], &CONVO[i]));
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
    reset_session_state();
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

    /* Loading is non-destructive: replay compatibility is decided later, so
     * explicit provenance survives and unstamped reasoning inherits the header. */
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
    reset_session_state();
    char *path = write_session("alpha", "m1", "high", NULL, CONVO, CONVO_N);
    struct item *items;
    size_t n;
    struct session_meta meta;
    EXPECT(session_load(path, &items, &n, &meta) == 0);
    char *saved_id = xstrdup(meta.id);
    free_items(items, n);
    session_meta_free(&meta);

    /* Enumeration stays cheap by deriving identity from the filename and
     * deferring prompt extraction until the picker asks for it. */
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
        EXPECT(found->first_prompt == NULL);
        char *prompt = session_first_prompt(found->path, 64);
        EXPECT(prompt != NULL && strstr(prompt, "hello world") != NULL);
        free(prompt);
    }

    session_list_free(list, list_n);
    free(saved_id);
    free(path);
}

static void test_session_file_permissions(void)
{
    reset_session_state();
    char *path = write_session("alpha", "m1", NULL, NULL, CONVO, CONVO_N);

    /* Transcripts can contain secrets and must never be group/world-readable. */
    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 0077) == 0);

    free(path);
}

static void test_resume_appends_only_new_items(void)
{
    reset_session_state();
    char *path = write_session("alpha", "m1", "high", NULL, CONVO, CONVO_N);
    struct item extended[CONVO_N + 2];
    memcpy(extended, CONVO, sizeof(CONVO));
    extended[CONVO_N] = (struct item){.kind = ITEM_TURN_BOUNDARY};
    extended[CONVO_N + 1] = (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)"again"};

    /* Resume receives the full in-memory history but must append only the
     * suffix beyond its persisted high-water mark. */
    struct session_log *log = session_log_resume(path, "alpha", "m1", "high", NULL, CONVO_N);
    EXPECT(log != NULL);
    session_log_append(log, extended, CONVO_N + 2);
    session_log_close(log);

    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == CONVO_N + 2);
    if (n == CONVO_N + 2) {
        EXPECT(items[n - 1].kind == ITEM_USER_MESSAGE);
        EXPECT_STR_EQ(items[n - 1].text, "again");
    }

    free_items(items, n);
    free(path);
}

static void test_first_prompt_labels(void)
{
    reset_session_state();
    struct item boundary[] = {{.kind = ITEM_TURN_BOUNDARY}};
    char *empty_path = write_session("pa", "ma", NULL, NULL, boundary, 1);
    char *prompt = session_first_prompt(empty_path, 64);
    EXPECT(prompt == NULL);
    free(prompt);
    free(empty_path);

    struct item compacted[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"condensed summary", .compact_seed = 1},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"continuing"},
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"real question"},
    };

    /* Compaction summaries are synthetic; picker labels must prefer the first
     * real prompt and identify seed-only histories without presenting it as one. */
    char *continued_path = write_session("pa", "ma", NULL, NULL, compacted, 4);
    prompt = session_first_prompt(continued_path, 64);
    EXPECT(prompt != NULL && strstr(prompt, "real question") != NULL);
    EXPECT(prompt == NULL || strstr(prompt, "condensed summary") == NULL);
    free(prompt);
    free(continued_path);

    char *seed_path = write_session("pa", "ma", NULL, NULL, compacted, 2);
    prompt = session_first_prompt(seed_path, 64);
    EXPECT(prompt != NULL);
    if (prompt)
        EXPECT_STR_EQ(prompt, "(compacted)");
    free(prompt);
    free(seed_path);
}

static void test_resume_repairs_torn_final_line(void)
{
    char path[] = "/tmp/hax_torn_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd < 0)
        return;

    /* A crash can leave a partial JSON record. Resume must terminate that
     * fragment before appending, or the next valid record is fused to it. */
    const char *torn = "{\"type\":\"session\",\"version\":1,\"provider\":\"pa\",\"model\":\"ma\"}\n"
                       "{\"kind\":\"turn_boundary\"}\n"
                       "{\"kind\":\"user\",\"text\":\"hi\"}\n"
                       "{\"kind\":\"assistant\",\"text\":\"hello\"}\n"
                       "{\"kind\":\"user\",\"text\":\"torn";
    EXPECT(write(fd, torn, strlen(torn)) == (ssize_t)strlen(torn));
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
    reset_session_state();
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

    /* A crash before tool completion must not replay an unanswerable call into
     * the provider on resume. */
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
    reset_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL);
    EXPECT(log != NULL);

    /* Merely preparing a log must not create empty session clutter; the first
     * append is the point at which the session becomes durable. */
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
    reset_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL);
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, UNDO_CONVERSATION, 9);

    /* Undo cuts at a turn boundary and resets the append high-water mark, so a
     * replacement turn follows the retained history without resurrecting data. */
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
    reset_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL);
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, UNDO_CONVERSATION, 9);

    /* Undoing every turn retains a valid header rather than deleting or
     * corrupting the session file. */
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
    reset_session_state();
    char *source_path = write_session("pa", "ma", "hi", NULL, UNDO_CONVERSATION, 9);
    struct item *items;
    size_t n;
    struct session_meta meta;
    EXPECT(session_load(source_path, &items, &n, &meta) == 0);
    char *source_id = xstrdup(meta.id);
    free_items(items, n);
    session_meta_free(&meta);

    /* A fork needs a fresh identity and inherited selection while preserving
     * both the source file and only the requested turn prefix. */
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
    reset_session_state();
    struct session_log *log = session_log_open("pa", "ma", "hi", "review");
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));

    /* Before materialization a switch corrects the pending header; afterwards
     * it is recorded only when an append proves the switch produced output. */
    session_log_set_meta(log, "pb", "mb", NULL, NULL);
    session_log_append(log, UNDO_CONVERSATION, 3);
    struct session_meta meta;
    EXPECT(session_read_meta(path, &meta) == 0);
    EXPECT_STR_EQ(meta.provider, "pb");
    EXPECT_STR_EQ(meta.model, "mb");
    EXPECT(meta.effort == NULL);
    EXPECT(meta.preset == NULL);
    session_meta_free(&meta);

    session_log_set_meta(log, "pc", "mc", "low", "review");
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
    session_log_set_meta(resumed, "pc", "md", NULL, NULL);
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

static void test_truncate_restates_live_selection(void)
{
    reset_session_state();
    struct session_log *log = session_log_open("pa", "ma", NULL, NULL);
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    session_log_append(log, UNDO_CONVERSATION, 6);
    session_log_set_meta(log, "pb", "mb", NULL, "stance");
    session_log_append(log, UNDO_CONVERSATION, 9);

    /* If undo removes the selection record, the next append must restate the
     * live selection rather than silently reverting future resumes. */
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

    /* Callers must be able to clean up uniformly after an unreadable path. */
    EXPECT(session_read_meta("/nonexistent/hax-session.jsonl", &meta) == -1);
    EXPECT(meta.provider == NULL && meta.id == NULL);
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
    EXPECT(write(fd, header, strlen(header)) == (ssize_t)strlen(header));
    for (int i = 0; i < IMAGE_REQUEST_MAX_COUNT + 1; i++) {
        char *line = xasprintf("{\"kind\":\"tool_result\",\"call_id\":\"c%d\",\"output\":\"r\","
                               "\"images\":[{\"mime\":\"image/png\",\"data\":\"QUJD\","
                               "\"width\":2,\"height\":1}]}\n",
                               i);
        EXPECT(write(fd, line, strlen(line)) == (ssize_t)strlen(line));
        free(line);
    }
    close(fd);

    /* Files from a writer with a higher limit must still resume within today's
     * request budget, degrading only overflow images to placeholders. */
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

int main(void)
{
    test_item_codec_round_trip();
    test_recording_control();
    test_session_round_trip();
    test_reasoning_provenance_round_trip();
    test_session_listing();
    test_session_file_permissions();
    test_resume_appends_only_new_items();
    test_first_prompt_labels();
    test_resume_repairs_torn_final_line();
    test_load_trims_dangling_tool_call();
    test_log_materialization();
    test_truncate_and_reappend();
    test_truncate_all_turns();
    test_fork_copies_prefix_without_touching_source();
    test_selection_metadata_tracks_productive_switches();
    test_truncate_restates_live_selection();
    test_read_meta_failure_initializes_output();
    test_load_enforces_image_count_cap();
    T_REPORT();
}
