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

static int item_eq(const struct item *a, const struct item *b)
{
    return a->kind == b->kind && streq0(a->text, b->text) && streq0(a->call_id, b->call_id) &&
           streq0(a->tool_name, b->tool_name) &&
           streq0(a->tool_arguments_json, b->tool_arguments_json) && streq0(a->output, b->output) &&
           streq0(a->reasoning_json, b->reasoning_json) &&
           streq0(a->reasoning_text, b->reasoning_text) && a->compact_seed == b->compact_seed;
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

    free(saved_id);
    free(path);
    T_REPORT();
}
