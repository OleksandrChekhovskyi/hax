/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "agent_core.h"
#include "compact.h"
#include "config.h"
#include "harness.h"
#include "session.h"
#include "util.h"

/* compact_over_threshold is the pure trigger predicate; everything else
 * (compact_should_auto, agent_compact) layers config + I/O on top of it. */
static void test_over_threshold(void)
{
    /* Unknown window (limit <= 0) or unreported ctx never triggers. */
    EXPECT(!compact_over_threshold(100, 0, 85));
    EXPECT(!compact_over_threshold(100, -1, 85));
    EXPECT(!compact_over_threshold(-1, 1000, 85));

    /* Below / at / above the percentage boundary. */
    EXPECT(!compact_over_threshold(8499, 10000, 85));
    EXPECT(compact_over_threshold(8500, 10000, 85));
    EXPECT(compact_over_threshold(9999, 10000, 85));

    /* 100% threshold only fires when fully at the window. */
    EXPECT(!compact_over_threshold(9999, 10000, 100));
    EXPECT(compact_over_threshold(10000, 10000, 100));
}

static void test_should_auto(void)
{
    /* Drive the config tiers via runtime overrides so the test is
     * hermetic regardless of env / file config. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "90");

    EXPECT(!compact_should_auto(8999, 10000)); /* 89.99% < 90% */
    EXPECT(compact_should_auto(9000, 10000));  /* exactly 90% */
    EXPECT(!compact_should_auto(9999, 0));     /* unknown window */

    /* Disabled via config: never auto-compacts, even when far over. */
    config_set_override("compact.auto", "0");
    EXPECT(!compact_should_auto(100000, 10000));

    /* Out-of-range threshold falls back to the 85% default. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "0");
    EXPECT(!compact_should_auto(8499, 10000));
    EXPECT(compact_should_auto(8500, 10000));
}

static void test_context_limit_resolution(void)
{
    /* Resolution order: manual config override → provider probe value →
     * model-catalog entry → 0 (unknown). The catalog tier reads a fixture
     * snapshot through the real catalog module. */
    unsetenv("HAX_CONTEXT_LIMIT");
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs("{\"openai\": {\"models\": {\"m\": {\"limit\": {\"context\": 64000}}}}}", f);
        fclose(f);
    }

    struct provider p = {.name = "x"};

    /* No probe value and no catalog identity: unknown. */
    EXPECT(compact_context_limit(&p, "m") == 0);

    /* The catalog fills in for a mapped provider — but only with a model
     * to key by, and only when the catalog knows it. */
    p.catalog_id = "openai";
    EXPECT(compact_context_limit(&p, "m") == 64000);
    EXPECT(compact_context_limit(&p, NULL) == 0);
    EXPECT(compact_context_limit(&p, "unknown-model") == 0);

    /* A probe value beats the catalog... */
    atomic_store(&p.context_limit, 32000);
    EXPECT(compact_context_limit(&p, "m") == 32000);

    /* ...and the manual override beats everything. */
    config_set_override("context_limit", "16k");
    EXPECT(compact_context_limit(&p, "m") == 16 * 1024);
    config_set_override("context_limit", NULL);
}

enum transaction_script {
    TRANSACTION_SUCCESS,
    TRANSACTION_RETRY,
    TRANSACTION_ERROR,
    TRANSACTION_NO_SUMMARY,
};

static enum transaction_script transaction_script;
static int transaction_turn;
static int transaction_cancel;
static int transaction_events;

static void transaction_emit(stream_cb cb, void *user, struct stream_event event)
{
    cb(&event, user);
}

static void transaction_emit_done(stream_cb cb, void *user, long input_tokens)
{
    struct stream_usage usage = {.input_tokens = input_tokens,
                                 .output_tokens = 10,
                                 .cached_tokens = -1,
                                 .cache_write_tokens = -1,
                                 .cache_write_1h_tokens = -1,
                                 .cost = -1};
    transaction_emit(cb, user, (struct stream_event){.kind = EV_DONE, .u.done = {.usage = usage}});
}

static int transaction_stream(struct provider *provider, const struct context *ctx,
                              const char *model, stream_cb cb, void *user, http_tick_cb tick,
                              void *tick_user)
{
    (void)provider;
    (void)model;
    (void)tick;
    (void)tick_user;
    EXPECT(ctx != NULL);
    transaction_turn++;

    if (transaction_script == TRANSACTION_RETRY && transaction_turn == 1) {
        transaction_emit(cb, user,
                         (struct stream_event){.kind = EV_TOOL_CALL_START,
                                               .u.tool_call_start = {.id = "c1", .name = "read"}});
        transaction_emit(
            cb, user,
            (struct stream_event){.kind = EV_TOOL_CALL_DELTA,
                                  .u.tool_call_delta = {.id = "c1", .args_delta = "{}"}});
        transaction_emit(
            cb, user,
            (struct stream_event){.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = "c1"}});
        transaction_emit_done(cb, user, 100);
        return 0;
    }
    if (transaction_script == TRANSACTION_ERROR) {
        static const struct stream_usage usage = {.input_tokens = 150,
                                                  .output_tokens = 5,
                                                  .cached_tokens = -1,
                                                  .cache_write_tokens = -1,
                                                  .cache_write_1h_tokens = -1,
                                                  .cost = -1};
        transaction_emit(
            cb, user,
            (struct stream_event){.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "partial"}});
        transaction_emit(
            cb, user,
            (struct stream_event){.kind = EV_ERROR,
                                  .u.error = {.message = "summary failed", .usage = &usage}});
        return 0;
    }
    if (transaction_script != TRANSACTION_NO_SUMMARY)
        transaction_emit(cb, user,
                         (struct stream_event){.kind = EV_TEXT_DELTA,
                                               .u.text_delta = {.text = "## Goal\n- continue"}});
    transaction_emit_done(cb, user, transaction_turn == 1 ? 100 : 200);
    return 0;
}

static int transaction_observe(const struct stream_event *event, void *user)
{
    (void)event;
    (void)user;
    transaction_events++;
    return 0;
}

static int transaction_cancelled(void *user)
{
    (void)user;
    return transaction_cancel;
}

static void transaction_session_init(struct agent_session *session)
{
    memset(session, 0, sizeof(*session));
    session->model = xstrdup("model");
    session->provider_name = "test";
    items_append(&session->items, &session->n_items, &session->cap_items,
                 (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("old history")});
}

static struct compact_result transaction_run(struct agent_session *session,
                                             struct provider *provider, struct session_log *slog)
{
    struct compact_params params = {
        .session = session,
        .provider = provider,
        .slog = slog,
        .hooks =
            {
                .observe = transaction_observe,
                .cancelled = transaction_cancelled,
            },
    };
    struct compact_result result;
    compact_run(&params, &result);
    return result;
}

static void test_transaction_applies_summary(void)
{
    struct agent_session session;
    transaction_session_init(&session);
    struct provider provider = {.name = "test", .stream = transaction_stream};
    transaction_script = TRANSACTION_SUCCESS;
    transaction_turn = 0;
    transaction_cancel = 0;
    transaction_events = 0;

    struct compact_result result = transaction_run(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 1);
    EXPECT(transaction_events == 2);
    /* Successful compaction leaves only the seed and its own usage footer. */
    EXPECT(session.n_items == 2);
    EXPECT(session.items[0].kind == ITEM_USER_MESSAGE && session.items[0].compact_seed);
    EXPECT(session.items[0].text && strstr(session.items[0].text, "earlier part") != NULL);
    EXPECT(session.items[0].text && strstr(session.items[0].text, "continue") != NULL);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 100);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void free_items(struct item *items, size_t n_items)
{
    for (size_t i = 0; i < n_items; i++)
        item_free(&items[i]);
    free(items);
}

static void test_transaction_archives_rejected_attempt(void)
{
    struct agent_session session;
    transaction_session_init(&session);
    struct provider provider = {.name = "test", .stream = transaction_stream};
    struct session_log *slog = session_log_open("test", "model", NULL);
    EXPECT(slog != NULL);
    session_log_append(slog, session.items, session.n_items);
    char *old_path = xstrdup(session_log_path(slog));
    transaction_script = TRANSACTION_RETRY;
    transaction_turn = 0;
    transaction_cancel = 0;

    struct compact_result result = transaction_run(&session, &provider, slog);
    EXPECT(result.outcome == COMPACT_COMPLETE);
    EXPECT(result.attempts == 2);
    /* The rejected turn is archived with old history; only the accepted
     * turn's footer follows the new seed. */
    struct item *old_items = NULL;
    size_t n_old = 0;
    EXPECT(session_load(old_path, &old_items, &n_old, NULL) == 0);
    EXPECT(n_old == 2);
    EXPECT(old_items[1].kind == ITEM_TURN_USAGE);
    EXPECT(old_items[1].usage->usage.input_tokens == 100);
    EXPECT(session.n_items == 2);
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 200);

    struct item *new_items = NULL;
    size_t n_new = 0;
    EXPECT(strcmp(old_path, session_log_path(slog)) != 0);
    EXPECT(session_load(session_log_path(slog), &new_items, &n_new, NULL) == 0);
    EXPECT(n_new == 2);
    EXPECT(new_items[0].kind == ITEM_USER_MESSAGE && new_items[0].compact_seed);
    EXPECT(new_items[1].kind == ITEM_TURN_USAGE);
    EXPECT(new_items[1].usage->usage.input_tokens == 200);

    free_items(new_items, n_new);
    free_items(old_items, n_old);
    free(old_path);
    compact_result_destroy(&result);
    session_log_close(slog);
    agent_session_free(&session);
}

static void test_transaction_preserves_failure(void)
{
    struct agent_session session;
    transaction_session_init(&session);
    struct provider provider = {.name = "test", .stream = transaction_stream};
    transaction_script = TRANSACTION_ERROR;
    transaction_turn = 0;
    transaction_cancel = 0;

    struct compact_result result = transaction_run(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_PROVIDER_ERROR);
    EXPECT_STR_EQ(result.error_message, "summary failed");
    /* Partial summary text never enters history; billed failure usage does. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);
    EXPECT(session.items[1].usage->usage.input_tokens == 150);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_transaction_rejects_empty_summary(void)
{
    struct agent_session session;
    transaction_session_init(&session);
    struct provider provider = {.name = "test", .stream = transaction_stream};
    transaction_script = TRANSACTION_NO_SUMMARY;
    transaction_turn = 0;
    transaction_cancel = 0;

    struct compact_result result = transaction_run(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_NO_SUMMARY);
    /* A clean but empty response is still a completed, billed attempt; it gets
     * a footer without replacing usable history. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

static void test_transaction_discards_cancelled_summary(void)
{
    struct agent_session session;
    transaction_session_init(&session);
    struct provider provider = {.name = "test", .stream = transaction_stream};
    transaction_script = TRANSACTION_SUCCESS;
    transaction_turn = 0;
    transaction_cancel = 1;

    struct compact_result result = transaction_run(&session, &provider, NULL);
    EXPECT(result.outcome == COMPACT_CANCELLED);
    /* A late cancel wins over a complete summary: old history survives and
     * the completed attempt still receives its footer. */
    EXPECT(session.n_items == 2);
    EXPECT_STR_EQ(session.items[0].text, "old history");
    EXPECT(session.items[1].kind == ITEM_TURN_USAGE);

    compact_result_destroy(&result);
    agent_session_free(&session);
}

int main(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
    test_over_threshold();
    test_should_auto();
    test_context_limit_resolution();
    test_transaction_applies_summary();
    test_transaction_archives_rejected_attempt();
    test_transaction_preserves_failure();
    test_transaction_rejects_empty_summary();
    test_transaction_discards_cancelled_summary();
    T_REPORT();
}
