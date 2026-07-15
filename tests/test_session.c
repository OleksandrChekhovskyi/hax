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
           a->cost_total == b->cost_total && a->cost_estimated == b->cost_estimated;
}

static int item_eq(const struct item *a, const struct item *b)
{
    return a->kind == b->kind && streq0(a->text, b->text) && streq0(a->call_id, b->call_id) &&
           streq0(a->tool_name, b->tool_name) &&
           streq0(a->tool_arguments_json, b->tool_arguments_json) && streq0(a->output, b->output) &&
           streq0(a->reasoning_json, b->reasoning_json) &&
           streq0(a->reasoning_text, b->reasoning_text) && a->compact_seed == b->compact_seed &&
           usage_eq(a->usage, b->usage);
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
    .cost_in = -1,
    .cost_cache_read = -1,
    .cost_cache_write = -1,
    .cost_out = -1,
    .cost_total = 0.0012,
    .cost_estimated = 0,
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

int main(void)
{
    /* ---- codec: every kind round-trips ---- */
    for (size_t i = 0; i < CONVO_N; i++)
        check_codec(&CONVO[i]);

    /* ---- isolate persistence under a throwaway state dir ---- */
    char tmpl[] = "/tmp/hax_sess_XXXXXX";
    char *tmp = mkdtemp(tmpl);
    EXPECT(tmp != NULL);
    if (!tmp)
        T_REPORT();
    setenv("XDG_STATE_HOME", tmp, 1);

    /* HAX_NO_SESSION disables persistence entirely. */
    setenv("HAX_NO_SESSION", "1", 1);
    EXPECT(session_log_open("alpha", "m1", "high") == NULL);
    unsetenv("HAX_NO_SESSION");

    /* ---- write a session, then load it back ---- */
    struct session_log *log = session_log_open("alpha", "m1", "high");
    EXPECT(log != NULL);
    char *path = xstrdup(session_log_path(log));
    EXPECT(path[0] != '\0');
    session_log_append(log, CONVO, CONVO_N);
    session_log_close(log);

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
    char *saved_id = xstrdup(meta.id);
    free_items(items, n);
    session_meta_free(&meta);

    /* ---- load is non-destructive: the opaque blob and its provenance stamp
       survive verbatim. Whether a blob can be replayed is decided later by the
       provider's build path (which compares the stamp to the request's model),
       not by load — so a model switch never destroys history here. ---- */
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == CONVO_N);
    const struct item *rs = NULL;
    for (size_t i = 0; i < n; i++)
        if (items[i].kind == ITEM_REASONING)
            rs = &items[i];
    EXPECT(rs && rs->reasoning_json != NULL && rs->reasoning_text != NULL);
    /* The fixture carries no per-item stamp, so it inherits the header's. */
    EXPECT(rs && rs->provider && strcmp(rs->provider, "alpha") == 0);
    EXPECT(rs && rs->model && strcmp(rs->model, "m1") == 0);
    free_items(items, n);

    /* ---- listing finds the session with a usable summary ---- */
    char cwd[4096];
    EXPECT(getcwd(cwd, sizeof(cwd)) != NULL);
    struct session_entry *list;
    size_t ln;
    EXPECT(session_list(cwd, &list, &ln) == 0);
    EXPECT(ln >= 1);
    const struct session_entry *found = NULL;
    for (size_t i = 0; i < ln; i++)
        if (strcmp(list[i].path, path) == 0)
            found = &list[i];
    EXPECT(found != NULL);
    if (found) {
        EXPECT_STR_EQ(found->id, saved_id);  /* id comes from the filename, no file read */
        EXPECT(found->first_prompt == NULL); /* listing is enumeration-only */
        char *fp = session_first_prompt(found->path, 64); /* lazily, on demand */
        EXPECT(fp != NULL && strstr(fp, "hello world") != NULL);
        free(fp);
    }
    session_list_free(list, ln);

    /* ---- session files are owner-only (no world-readable secrets) ---- */
    struct stat pst;
    EXPECT(stat(path, &pst) == 0);
    EXPECT((pst.st_mode & 0077) == 0); /* no group/other permissions */

    /* ---- resume continues the same file ---- */
    struct item convo_ext[CONVO_N + 2];
    memcpy(convo_ext, CONVO, sizeof(CONVO));
    convo_ext[CONVO_N] = (struct item){.kind = ITEM_TURN_BOUNDARY};
    convo_ext[CONVO_N + 1] = (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)"again"};

    struct session_log *r = session_log_resume(path, "alpha", "m1", "high", CONVO_N);
    EXPECT(r != NULL);
    session_log_append(r, convo_ext, CONVO_N + 2);
    session_log_close(r);

    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == CONVO_N + 2);
    if (n == CONVO_N + 2) {
        EXPECT(items[n - 1].kind == ITEM_USER_MESSAGE);
        EXPECT_STR_EQ(items[n - 1].text, "again");
    }
    free_items(items, n);

    /* ---- the reasoning provenance stamp round-trips: a per-item stamp wins,
       and an item lacking one inherits the header's ---- */
    struct item conv_r[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"q"},
        /* explicit per-item stamp (a different model than the header's) */
        {.kind = ITEM_REASONING,
         .reasoning_json = (char *)"{\"id\":\"enc\"}",
         .provider = (char *)"pa",
         .model = (char *)"mX"},
        /* no stamp → inherits the header (pa/ma) */
        {.kind = ITEM_REASONING, .reasoning_text = (char *)"plain cot"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"a"},
    };
    struct session_log *lr = session_log_open("pa", "ma", NULL);
    EXPECT(lr != NULL);
    char *pathr = xstrdup(session_log_path(lr));
    session_log_append(lr, conv_r, 4);
    session_log_close(lr);

    EXPECT(session_load(pathr, &items, &n, NULL) == 0);
    const struct item *enc = NULL, *txt = NULL;
    for (size_t i = 0; i < n; i++)
        if (items[i].kind == ITEM_REASONING) {
            if (items[i].reasoning_json)
                enc = &items[i];
            else
                txt = &items[i];
        }
    /* Both items survive (non-destructive); the blob keeps its own stamp... */
    EXPECT(enc && enc->reasoning_json && enc->provider && strcmp(enc->provider, "pa") == 0);
    EXPECT(enc && enc->model && strcmp(enc->model, "mX") == 0);
    /* ...and the unstamped one inherits the header. */
    EXPECT(txt && txt->reasoning_text && strcmp(txt->reasoning_text, "plain cot") == 0);
    EXPECT(txt && txt->model && strcmp(txt->model, "ma") == 0);
    free_items(items, n);

    free(pathr);

    /* ---- a session with no user message has no first prompt ---- */
    struct item conv_b[] = {{.kind = ITEM_TURN_BOUNDARY}};
    struct session_log *lb = session_log_open("pa", "ma", NULL);
    EXPECT(lb != NULL);
    char *pathb = xstrdup(session_log_path(lb));
    session_log_append(lb, conv_b, 1);
    session_log_close(lb);
    char *empty_fp = session_first_prompt(pathb, 64);
    EXPECT(empty_fp == NULL);
    free(empty_fp);
    free(pathb);

    /* ---- the first-prompt label skips a compaction seed ---- */
    /* A compacted-then-continued session labels by the first real prompt;
     * one holding only the seed labels "(compacted)". */
    struct item conv_seed[] = {
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"condensed summary", .compact_seed = 1},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"continuing"},
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"real question"},
    };
    struct session_log *ls = session_log_open("pa", "ma", NULL);
    EXPECT(ls != NULL);
    char *paths = xstrdup(session_log_path(ls));
    session_log_append(ls, conv_seed, 4);
    session_log_close(ls);
    char *seed_fp = session_first_prompt(paths, 64);
    EXPECT(seed_fp != NULL && strstr(seed_fp, "real question") != NULL);
    EXPECT(seed_fp == NULL || strstr(seed_fp, "condensed summary") == NULL);
    free(seed_fp);
    free(paths);

    struct session_log *lso = session_log_open("pa", "ma", NULL);
    EXPECT(lso != NULL);
    char *pathso = xstrdup(session_log_path(lso));
    session_log_append(lso, conv_seed, 2); /* seed + assistant, no real prompt */
    session_log_close(lso);
    char *only_fp = session_first_prompt(pathso, 64);
    EXPECT(only_fp != NULL);
    if (only_fp)
        EXPECT_STR_EQ(only_fp, "(compacted)");
    free(only_fp);
    free(pathso);

    /* ---- resuming repairs a torn final line instead of fusing onto it ---- */
    /* Simulate a crash that left the last record half-written (no trailing
     * newline). A naive append would concatenate the next record onto it
     * and corrupt the file; the resume path must terminate it first. */
    char tornpath[] = "/tmp/hax_torn_XXXXXX";
    int tfd = mkstemp(tornpath);
    EXPECT(tfd >= 0);
    if (tfd >= 0) {
        const char *torn =
            "{\"type\":\"session\",\"version\":1,\"provider\":\"pa\",\"model\":\"ma\"}\n"
            "{\"kind\":\"turn_boundary\"}\n"
            "{\"kind\":\"user\",\"text\":\"hi\"}\n"
            "{\"kind\":\"assistant\",\"text\":\"hello\"}\n"
            "{\"kind\":\"user\",\"text\":\"torn"; /* no close, no newline */
        EXPECT(write(tfd, torn, strlen(torn)) == (ssize_t)strlen(torn));
        close(tfd);

        struct item *base;
        size_t nb;
        EXPECT(session_load(tornpath, &base, &nb, NULL) == 0);
        EXPECT(nb == 3); /* boundary, user, assistant — torn line skipped */

        struct item ext[5];
        memcpy(ext, base, nb * sizeof(struct item));
        ext[nb] = (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)"after crash"};
        struct session_log *lt = session_log_resume(tornpath, "pa", "ma", NULL, nb);
        EXPECT(lt != NULL);
        session_log_append(lt, ext, nb + 1);
        session_log_close(lt);
        free_items(base, nb);

        /* The appended record must be intact, not fused onto the fragment. */
        struct item *after;
        size_t na;
        EXPECT(session_load(tornpath, &after, &na, NULL) == 0);
        EXPECT(na == 4); /* the 3 valid + the new one; torn fragment still skipped */
        if (na == 4)
            EXPECT_STR_EQ(after[3].text, "after crash");
        free_items(after, na);
        unlink(tornpath);
    }

    /* ---- a dangling tool_call (crash before its result) is trimmed ---- */
    struct item conv_tc[] = {
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = (char *)"run it"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = (char *)"sure"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = (char *)"c1",
         .tool_name = (char *)"bash",
         .tool_arguments_json = (char *)"{}"}, /* no matching tool_result */
    };
    struct session_log *ltc = session_log_open("pa", "ma", NULL);
    EXPECT(ltc != NULL);
    char *pathtc = xstrdup(session_log_path(ltc));
    session_log_append(ltc, conv_tc, 4);
    session_log_close(ltc);
    EXPECT(session_load(pathtc, &items, &n, NULL) == 0);
    EXPECT(n == 3); /* boundary, user, assistant — the unpaired call dropped */
    for (size_t i = 0; i < n; i++)
        EXPECT(items[i].kind != ITEM_TOOL_CALL);
    free_items(items, n);
    free(pathtc);

    /* ---- session_log_materialized flips on the first append ---- */
    struct session_log *lm = session_log_open("pa", "ma", NULL);
    EXPECT(lm != NULL);
    EXPECT(session_log_materialized(lm) == 0); /* path assigned, file not written yet */
    struct item one_turn[] = {{.kind = ITEM_TURN_BOUNDARY},
                              {.kind = ITEM_USER_MESSAGE, .text = (char *)"hi"}};
    session_log_append(lm, one_turn, 2);
    EXPECT(session_log_materialized(lm) != 0); /* header + items now on disk */
    session_log_close(lm);
    EXPECT(session_log_materialized(NULL) == 0);

    /* ---- undo: session_log_truncate keeps the first N user turns ---- */
    /* Three turns; each is boundary + user + assistant. */
    struct item conv_u[] = {
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
    struct session_log *lu = session_log_open("pa", "ma", NULL);
    EXPECT(lu != NULL);
    char *pathu = xstrdup(session_log_path(lu));
    session_log_append(lu, conv_u, 9);
    /* Keep 2 turns: the cut lands on the boundary opening turn index 2, so
     * items [0,6) survive (through assistant "a1"). */
    EXPECT(session_log_truncate(lu, 2, 6) == 0);
    /* Appending resumes cleanly from the new high-water mark. */
    struct item conv_u2[7];
    memcpy(conv_u2, conv_u, 6 * sizeof(struct item));
    conv_u2[6] = (struct item){.kind = ITEM_USER_MESSAGE, .text = (char *)"redo"};
    session_log_append(lu, conv_u2, 7);
    session_log_close(lu);

    EXPECT(session_load(pathu, &items, &n, NULL) == 0);
    EXPECT(n == 7);
    if (n == 7) {
        EXPECT_STR_EQ(items[5].text, "a1");
        EXPECT_STR_EQ(items[6].text, "redo");
    }
    for (size_t i = 0; i < n; i++)
        EXPECT(!(items[i].text && strcmp(items[i].text, "t2") == 0)); /* discarded turn gone */
    free_items(items, n);
    free(pathu);

    /* ---- undo everything: keep 0 turns leaves just the header ---- */
    struct session_log *lz = session_log_open("pa", "ma", NULL);
    EXPECT(lz != NULL);
    char *pathz = xstrdup(session_log_path(lz));
    session_log_append(lz, conv_u, 9);
    EXPECT(session_log_truncate(lz, 0, 0) == 0);
    session_log_close(lz);
    EXPECT(session_load(pathz, &items, &n, NULL) == 0);
    EXPECT(n == 0);
    free_items(items, n);
    free(pathz);

    /* ---- fork: prefix copy branches without touching the source ---- */
    struct session_log *lf = session_log_open("pa", "ma", "hi");
    EXPECT(lf != NULL);
    char *pathf = xstrdup(session_log_path(lf));
    session_log_append(lf, conv_u, 9);
    session_log_close(lf);

    /* Capture the source id to prove the fork gets a fresh one. */
    EXPECT(session_load(pathf, &items, &n, &meta) == 0);
    char *src_id = xstrdup(meta.id);
    free_items(items, n);
    session_meta_free(&meta);

    char *forkpath = NULL;
    EXPECT(session_fork_file(pathf, 1, &forkpath) == 0);
    EXPECT(forkpath != NULL);
    if (forkpath) {
        EXPECT(session_load(forkpath, &items, &n, &meta) == 0);
        EXPECT(n == 3); /* one turn: boundary, user, assistant */
        if (n == 3)
            EXPECT_STR_EQ(items[1].text, "t0");
        for (size_t i = 0; i < n; i++)
            EXPECT(!(items[i].text && strcmp(items[i].text, "t1") == 0));
        /* Fresh identity, inherited settings. */
        EXPECT(meta.id != NULL && strcmp(meta.id, src_id) != 0);
        EXPECT_STR_EQ(meta.provider, "pa");
        EXPECT_STR_EQ(meta.model, "ma");
        EXPECT_STR_EQ(meta.effort, "hi");
        free_items(items, n);
        session_meta_free(&meta);

        /* The header records where it forked from. */
        size_t flen;
        char *fdata = slurp_file(forkpath, &flen);
        EXPECT(fdata != NULL);
        if (fdata) {
            EXPECT(strstr(fdata, "forked_from") != NULL);
            EXPECT(strstr(fdata, src_id) != NULL);
            free(fdata);
        }
        free(forkpath);
    }

    /* The source file is untouched — still three full turns. */
    EXPECT(session_load(pathf, &items, &n, NULL) == 0);
    EXPECT(n == 9);
    free_items(items, n);

    /* Forking past the last turn clones the whole file. */
    char *clonepath = NULL;
    EXPECT(session_fork_file(pathf, 3, &clonepath) == 0);
    if (clonepath) {
        EXPECT(session_load(clonepath, &items, &n, NULL) == 0);
        EXPECT(n == 9);
        free_items(items, n);
        free(clonepath);
    }
    free(src_id);
    free(pathf);

    free(saved_id);
    free(path);
    T_REPORT();
}
