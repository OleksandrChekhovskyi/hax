/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "agent.h"
#include "agent_core.h"
#include "agent_usage.h"
#include "config.h"
#include "effort.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "xalloc.h"
#include "render/render_ctx.h"

/* Run `body` with captured stdout, restore stdout, and return owned output. */
static char *capture_stdout(void (*body)(void *), void *user)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    EXPECT(saved >= 0);

    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    int tmpfd = fileno(tmp);
    EXPECT(dup2(tmpfd, STDOUT_FILENO) >= 0);

    body(user);

    fflush(stdout);
    EXPECT(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);

    EXPECT(fseek(tmp, 0, SEEK_END) == 0);
    long n = ftell(tmp);
    EXPECT(n >= 0);
    EXPECT(fseek(tmp, 0, SEEK_SET) == 0);
    char *buf = xmalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, tmp);
    buf[got] = '\0';
    fclose(tmp);
    return buf;
}

/* ---------- agent_apply_settings: banner / marker split ---------- */

/* Environment overrides isolate settings resolution from the user's configuration. */
struct fixture {
    struct provider provider;
    struct agent_session session;
    struct render_ctx render;
    struct agent_state state;
    struct provider *candidate;
    int result;
};

static void fixture_init(struct fixture *f)
{
    /* Clear every registered binding before pinning this fixture's: the
     * assertions below name an exact selection, and agent_provider_id
     * resolves HAX_PROVIDER — which hax exports into every subagent, so a
     * suite run from inside hax (or any shell with one set) would otherwise
     * record the caller's provider instead of prov-x. */
    size_t n_settings = 0;
    const struct config_setting *settings = config_settings(&n_settings);
    for (size_t i = 0; i < n_settings; i++)
        unsetenv(settings[i].env_var);

    setenv("HAX_MODEL", "model-a", 1);
    setenv("HAX_SYSTEM_PROMPT", "sys", 1);
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    unsetenv("HAX_EFFORT");

    memset(f, 0, sizeof(*f));
    f->provider.name = "prov-x";
    struct hax_opts opts = {0};
    agent_session_init(&f->session, &f->provider, &opts);

    /* Model the dispatcher's state at the point select.c calls apply:
     * the leading-gap separator has run, cursor on a blank line. */
    f->render.disp.committed_newlines = 2;
    f->state.session = &f->session;
    f->state.provider = &f->provider;
    f->state.render = &f->render;
    f->candidate = &f->provider;
}

static void fixture_free(struct fixture *f)
{
    /* These stub providers stand in for real ones, so they owe the same
     * teardown: agent_apply_settings may have started a metadata fetch on
     * whichever provider ended up live, and provider.h puts the release in
     * destroy(). The fixtures have no destroy of their own. */
    model_meta_release(&f->provider);
    if (f->state.provider && f->state.provider != &f->provider)
        model_meta_release(f->state.provider);
    agent_session_free(&f->session);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void do_apply(void *user)
{
    struct fixture *f = user;
    f->result = agent_apply_settings(&f->state, f->candidate, 1);
}

static void test_apply_settings_empty_reprints_banner(void)
{
    struct fixture f;
    fixture_init(&f);
    EXPECT(f.session.n_items == 0);

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.result == 0);
    EXPECT(strstr(out, "hax") != NULL);
    EXPECT(strstr(out, "prov-x · model-a") != NULL);
    EXPECT(strstr(out, "ctrl-d quit") != NULL);
    EXPECT(strstr(out, "switched to") == NULL);
    /* The banner bypasses disp, so the branch must record its committed newline. */
    EXPECT(f.render.disp.committed_newlines == 1);

    free(out);
    fixture_free(&f);
}

static void test_apply_settings_nonempty_prints_marker(void)
{
    struct fixture f;
    fixture_init(&f);
    agent_session_add_user(&f.session, "hello");
    EXPECT(f.session.n_items > 0);

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.result == 0);
    EXPECT(strstr(out, "switched to prov-x · model-a") != NULL);
    EXPECT(strstr(out, "ctrl-d quit") == NULL);
    /* The marker uses disp, so its trailing newline remains pending. */
    EXPECT(f.render.disp.committed_newlines == 0);
    EXPECT(f.render.disp.pending_newlines == 1);

    free(out);
    fixture_free(&f);
}

static void do_apply_quiet(void *user)
{
    struct fixture *f = user;
    f->result = agent_apply_settings(&f->state, f->candidate, 0);
}

static void test_apply_settings_quiet_prints_nothing(void)
{
    /* `/new <preset>` applies before it resets and prints its own banner
     * afterwards, so announce = 0 must produce no output at all — not the
     * mid-conversation marker this history would otherwise earn — while
     * still applying the settings. disp is left exactly as the caller set
     * it, so the banner that follows draws into the same gap. */
    struct fixture f;
    fixture_init(&f);
    agent_session_add_user(&f.session, "hello");
    EXPECT(f.session.n_items > 0);
    setenv("HAX_MODEL", "model-b", 1); /* the change the silent apply resolves */

    char *out = capture_stdout(do_apply_quiet, &f);
    EXPECT(f.result == 0);
    EXPECT_STR_EQ(out, "");
    /* Silence is about output, not effect. */
    EXPECT_STR_EQ(f.session.model, "model-b");
    EXPECT(f.render.disp.committed_newlines == 2);
    EXPECT(f.render.disp.pending_newlines == 0);

    free(out);
    fixture_free(&f);
}

static void test_apply_settings_no_model_fails_intact(void)
{
    struct fixture f;
    fixture_init(&f);
    agent_session_add_user(&f.session, "hello");
    size_t items_before = f.session.n_items;
    char *model_before = xstrdup(f.session.model);

    /* Pull the model out from under the next resolve: no env value and no
     * provider default. reconfigure must fail without touching history or
     * the currently-applied model, and print no confirmation. (Its "no
     * model available" diagnostic goes to stderr and shows in the test
     * log — expected, not a failure.) */
    unsetenv("HAX_MODEL");
    f.provider.default_model = NULL;

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.result == -1);
    EXPECT(f.session.n_items == items_before);
    EXPECT_STR_EQ(f.session.model, model_before);
    EXPECT(strstr(out, "switched to") == NULL);
    EXPECT(strstr(out, "ctrl-d quit") == NULL);

    free(out);
    free(model_before);
    fixture_free(&f);
}

/* ---------- agent_apply_settings: metadata refresh gate ---------- */

static int refresh_calls;
static int provider_destroy_calls;
static char refresh_last_model[64];

static void counting_provider_destroy(struct provider *p)
{
    (void)p;
    provider_destroy_calls++;
}

/* model_meta_refresh asks the provider to describe the fetch; counting the
 * asks is how we observe that a switch re-resolved the model's metadata.
 * Returning -1 ("nothing to fetch") keeps the test off the network. */
static int counting_probe_model(struct provider *p, const char *model, struct model_probe *out)
{
    (void)p;
    (void)out;
    refresh_calls++;
    snprintf(refresh_last_model, sizeof(refresh_last_model), "%s", model ? model : "");
    return -1;
}

static void test_apply_settings_failed_provider_change_keeps_old(void)
{
    struct fixture f;
    fixture_init(&f);
    unsetenv("HAX_MODEL");
    f.provider.destroy = counting_provider_destroy;
    struct provider next = {.name = "prov-y"};
    f.candidate = &next;
    provider_destroy_calls = 0;

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.result == -1);
    EXPECT(f.state.provider == &f.provider);
    EXPECT(provider_destroy_calls == 0);
    EXPECT_STR_EQ(f.session.provider_id, "prov-x");
    EXPECT(out[0] == '\0');

    free(out);
    fixture_free(&f);
}

static void test_apply_settings_refreshes_on_model_or_provider_change(void)
{
    struct fixture f;
    fixture_init(&f);
    f.provider.probe_model = counting_probe_model;
    refresh_calls = 0;

    /* Same model re-applied (the /effort-tweak shape): the metadata fetch
     * must not re-run — re-probing on every apply would add a needless
     * network round-trip and cancel/join churn. */
    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.result == 0);
    EXPECT(refresh_calls == 0);
    free(out);

    /* A real model change re-probes, with the new model. */
    setenv("HAX_MODEL", "model-b", 1);
    out = capture_stdout(do_apply, &f);
    EXPECT(f.result == 0);
    EXPECT(refresh_calls == 1);
    EXPECT_STR_EQ(refresh_last_model, "model-b");
    free(out);

    /* Provider identity is independently load-bearing: a fresh provider may
     * have skipped its constructor probe or probed a default model. Even when
     * the selected model string stays identical, refresh it once after the
     * ownership swap. */
    struct provider next = {
        .name = "prov-y",
        .probe_model = counting_probe_model,
    };
    f.provider.destroy = counting_provider_destroy;
    f.candidate = &next;
    refresh_calls = 0;
    provider_destroy_calls = 0;
    out = capture_stdout(do_apply, &f);
    EXPECT(f.result == 0);
    EXPECT(f.state.provider == &next);
    EXPECT(provider_destroy_calls == 1);
    EXPECT(refresh_calls == 1);
    EXPECT_STR_EQ(refresh_last_model, "model-b");
    free(out);

    fixture_free(&f);
}

/* ---------- agent_session_resync_effort ---------- */

static const char *const RESYNC_LADDER[] = {"low", "medium", "high", "max"};

static size_t resync_list_efforts(struct provider *p, const char *const **out)
{
    (void)p;
    *out = RESYNC_LADDER;
    return sizeof(RESYNC_LADDER) / sizeof(RESYNC_LADDER[0]);
}

/* The probe behind the narrowing runs in the background, so a session
 * resolves its effort before any answer exists. The next user turn has to
 * pick the answer up — nothing else re-resolves until the user switches
 * something. */
static void test_resync_effort_follows_late_metadata(void)
{
    struct fixture f;
    fixture_init(&f);
    f.provider.list_efforts = resync_list_efforts;
    setenv("HAX_EFFORT", "max", 1);

    /* Where a run starts: a ladder, nothing narrowing it, so the configured
     * level stands. */
    EXPECT(agent_session_resync_effort(&f.session, &f.provider, NULL) == 1);
    EXPECT_STR_EQ(f.session.effort, "max");
    char *prev = (char *)"sentinel";
    EXPECT(agent_session_resync_effort(&f.session, &f.provider, &prev) == 0);
    EXPECT(prev == NULL); /* nothing replaced, nothing handed back */

    /* What a landing probe publishes: this model stops at "high". */
    struct model_info m;
    model_info_init(&m);
    m.id = xstrdup("model-a");
    effort_set_add(&m.efforts, "low");
    effort_set_add(&m.efforts, "medium");
    effort_set_add(&m.efforts, "high");
    model_meta_store(&f.provider, &m);
    model_info_clear(&m);

    /* The replaced value comes back for the note the REPL prints: the
     * banner overhead is still asserting it. */
    EXPECT(agent_session_resync_effort(&f.session, &f.provider, &prev) == 1);
    EXPECT_STR_EQ(f.session.effort, "high");
    EXPECT_STR_EQ(prev, "max");
    free(prev);
    EXPECT(agent_session_resync_effort(&f.session, &f.provider, NULL) == 0);

    unsetenv("HAX_EFFORT");
    fixture_free(&f);
}

/* ---------- agent_apply_settings: spend records survive switches ---------- */

static void test_apply_settings_keeps_stamped_spend(void)
{
    struct fixture f;
    fixture_init(&f);

    /* Catalog fixture that knows only the OUTGOING model: after the
     * switch, requests recorded under model-a must keep pricing at
     * model-a's rates — the record's stamp, not the live model, decides. */
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *cf = fopen(path, "w");
    EXPECT(cf != NULL);
    if (cf) {
        fputs("{\"prov\": {\"models\": {"
              "\"model-a\": {\"cost\": {\"input\": 2, \"output\": 8}}}}}",
              cf);
        fclose(cf);
    }
    f.provider.catalog_id = "prov";
    struct stream_usage u = {.input_tokens = 1000000,
                             .output_tokens = 1000000,
                             .cached_tokens = -1,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = -1};
    agent_spend_account(&f.state.stats.spend, &u, &f.provider, "model-a");

    setenv("HAX_MODEL", "model-b", 1); /* model-a -> model-b */
    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.result == 0);
    int approx = 0;
    EXPECT(agent_session_spend(&f.state.stats, &approx) == 10.0); /* 1M*$2 + 1M*$8 per Mtok */
    EXPECT(approx == 1);
    free(out);

    /* A record whose stamp resolves nowhere (catalog fetch never landed,
     * unknown model) leaves the total at the reported subtotal, marked
     * approximate — it's missing real usage. */
    struct provider nowhere = {.catalog_id = "no-such-catalog-provider"};
    agent_spend_account(&f.state.stats.spend, &u, &nowhere, "model-x");
    struct stream_usage paid = {-1, -1, -1, -1, -1, 0.03};
    agent_spend_account(&f.state.stats.spend, &paid, &f.provider, "model-a");
    approx = 0;
    EXPECT(agent_session_spend(&f.state.stats, &approx) == 10.03);
    EXPECT(approx == 1);

    agent_spend_free(&f.state.stats.spend);
    fixture_free(&f);
}

/* ---------- agent_new_conversation ---------- */

static void do_new_conversation(void *user)
{
    struct fixture *f = user;
    agent_new_conversation(&f->state);
}

static void test_new_conversation_resets_everything(void)
{
    struct fixture f;
    fixture_init(&f);

    /* Seed every per-conversation accumulator /new promises to clear. */
    agent_session_add_user(&f.session, "hello");
    f.state.stats.user_turns = 3;
    f.state.stats.requests = 7;
    f.state.stats.input_tokens = 1000;
    f.state.stats.tool_calls = 2;

    char *out = capture_stdout(do_new_conversation, &f);
    EXPECT(f.session.n_items == 0);
    struct session_stats zero = {0};
    EXPECT(memcmp(&f.state.stats, &zero, sizeof(zero)) == 0);
    /* The fresh start is announced with the same banner as startup. */
    EXPECT(strstr(out, "prov-x · model-a") != NULL);
    EXPECT(strstr(out, "ctrl-d quit") != NULL);

    free(out);
    fixture_free(&f);
}

/* ---------- agent_undo / agent_fork: the real mutators ---------- */

static void add_turn(struct agent_session *session, const char *prompt, const char *reply)
{
    agent_session_add_user(session, prompt);
    agent_session_append(session,
                         (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup(reply)});
}

static void set_state_dir(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
}

static size_t count_users(const struct item *items, size_t item_count)
{
    size_t user_count = 0;
    for (size_t i = 0; i < item_count; i++)
        if (items[i].kind == ITEM_USER_MESSAGE && items[i].origin != ITEM_ORIGIN_COMPACT_SEED)
            user_count++;
    return user_count;
}

static void free_items(struct item *items, size_t item_count)
{
    for (size_t i = 0; i < item_count; i++)
        item_free(&items[i]);
    free(items);
}

struct history_mutation_call {
    struct agent_state *state;
    size_t turn_index;
};

static void do_undo(void *user)
{
    struct history_mutation_call *c = user;
    agent_undo(c->state, c->turn_index);
}

static void do_fork(void *user)
{
    struct history_mutation_call *c = user;
    agent_fork(c->state, c->turn_index);
}

/* An empty send after a marked stop appends CONTINUE_MARKER as a user item,
 * but it is not a prompt: it extends the turn already counted. Counting it
 * would inflate the history banner and offer /undo a revert point the user
 * never typed. */
static void test_continue_marker_is_not_a_user_turn(void)
{
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    agent_session_add_continuation(&f.session);
    agent_session_append(
        &f.session, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("r1 continued")});
    add_turn(&f.session, "second", "r2");

    EXPECT(agent_user_turn_count(&f.session) == 2);
    EXPECT_STR_EQ(agent_user_turn_text(&f.session, 0), "first");
    EXPECT_STR_EQ(agent_user_turn_text(&f.session, 1), "second");

    /* The same text typed by hand is a prompt like any other — provenance is
     * the item's flag, not its bytes, so a user who writes "[continue]" gets a
     * turn they can /undo back to. */
    agent_session_add_user(&f.session, CONTINUE_MARKER);
    EXPECT(agent_user_turn_count(&f.session) == 3);
    EXPECT_STR_EQ(agent_user_turn_text(&f.session, 2), CONTINUE_MARKER);
    fixture_free(&f);
}

static void test_undo_reverts_history_and_file(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    add_turn(&f.session, "second", "r2");
    add_turn(&f.session, "third", "r3");

    f.state.session_log = session_log_open("prov-x", "model-a", NULL, NULL, NULL);
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    char *path = xstrdup(session_log_path(f.state.session_log));
    EXPECT(agent_user_turn_count(&f.session) == 3);

    /* Revert to before turn 1: turn 0 survives, turns 1 and 2 drop. */
    struct history_mutation_call c = {.state = &f.state, .turn_index = 1};
    char *out = capture_stdout(do_undo, &c);

    EXPECT(agent_user_turn_count(&f.session) == 1);
    EXPECT_STR_EQ(agent_user_turn_text(&f.session, 0), "first");
    /* The discarded prompt is staged for editor recall. */
    EXPECT_STR_EQ(f.state.pending_recall, "second");

    /* The truncation reached disk: reloading shows only turn 0. */
    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(count_users(items, n) == 1);
    free_items(items, n);

    free(out);
    free(path);
    free(f.state.pending_recall);
    session_log_close(f.state.session_log);
    agent_session_free(&f.session);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

/* The in-memory cut and the on-disk cut have to land on the same turn. /undo
 * passes a turn number derived from the item scan and an item count derived
 * from it too; if the file scan counts a continuation marker as a turn, it
 * cuts a turn earlier, and the item count then recorded as written skips
 * every line in between — the log silently loses the retained tail and
 * everything appended after it. */
static void test_undo_with_continuation_cuts_disk_and_memory_alike(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    /* An interrupted first turn resumed by an empty send. */
    agent_session_add_continuation(&f.session);
    agent_session_append(
        &f.session, (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("r1 continued")});
    add_turn(&f.session, "second", "r2");

    f.state.session_log = session_log_open("prov-x", "model-a", NULL, NULL, NULL);
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    char *path = xstrdup(session_log_path(f.state.session_log));
    EXPECT(agent_user_turn_count(&f.session) == 2);

    /* Revert to before "second": the whole first turn, continuation included,
     * survives in memory... */
    struct history_mutation_call c = {.state = &f.state, .turn_index = 1};
    char *out = capture_stdout(do_undo, &c);
    EXPECT(agent_user_turn_count(&f.session) == 1);
    size_t kept = f.session.n_items;

    /* ...and on disk, item for item. */
    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == kept);
    int saw_continuation = 0;
    for (size_t i = 0; i < n; i++)
        if (items[i].origin == ITEM_ORIGIN_CONTINUATION)
            saw_continuation = 1;
    EXPECT(saw_continuation);
    free_items(items, n);

    /* A later append lands after the retained tail rather than overwriting
     * it: n_written must match what the file actually holds. */
    add_turn(&f.session, "third", "r3");
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(n == f.session.n_items);
    free_items(items, n);

    free(out);
    free(path);
    free(f.state.pending_recall);
    session_log_close(f.state.session_log);
    agent_session_free(&f.session);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_fork_branches_and_switches_log(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    add_turn(&f.session, "second", "r2");
    add_turn(&f.session, "third", "r3");

    f.state.session_log = session_log_open("prov-x", "model-a", NULL, NULL, NULL);
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    char *orig = xstrdup(session_log_path(f.state.session_log));

    struct history_mutation_call c = {.state = &f.state, .turn_index = 1};
    char *out = capture_stdout(do_fork, &c);

    /* History cut to the branch point, discarded prompt staged. */
    EXPECT(agent_user_turn_count(&f.session) == 1);
    EXPECT_STR_EQ(f.state.pending_recall, "second");

    /* The live log moved to a new file... */
    const char *newpath = session_log_path(f.state.session_log);
    EXPECT(newpath != NULL);
    EXPECT(strcmp(newpath, orig) != 0);

    /* ...which holds just the branch prefix, stamped forked_from the source... */
    struct item *items;
    size_t n;
    struct session_meta meta = {0};
    EXPECT(session_load(newpath, &items, &n, &meta) == 0);
    EXPECT(count_users(items, n) == 1);
    free_items(items, n);
    session_meta_free(&meta);

    /* ...while the original is left whole and resumable. */
    EXPECT(session_load(orig, &items, &n, NULL) == 0);
    EXPECT(count_users(items, n) == 3);
    free_items(items, n);

    free(out);
    free(orig);
    free(f.state.pending_recall);
    session_log_close(f.state.session_log);
    agent_session_free(&f.session);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_fork_at_tip_clones_whole(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    add_turn(&f.session, "second", "r2");

    f.state.session_log = session_log_open("prov-x", "model-a", NULL, NULL, NULL);
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    char *orig = xstrdup(session_log_path(f.state.session_log));
    size_t items_before = f.session.n_items;

    /* turn == count: clone at the tip, nothing discarded. */
    struct history_mutation_call c = {.state = &f.state, .turn_index = 2};
    char *out = capture_stdout(do_fork, &c);

    EXPECT(f.session.n_items == items_before);
    EXPECT(f.state.pending_recall == NULL); /* no prompt discarded */

    const char *newpath = session_log_path(f.state.session_log);
    EXPECT(strcmp(newpath, orig) != 0);
    struct item *items;
    size_t n;
    EXPECT(session_load(newpath, &items, &n, NULL) == 0);
    EXPECT(count_users(items, n) == 2); /* full clone */
    free_items(items, n);

    free(out);
    free(orig);
    session_log_close(f.state.session_log);
    agent_session_free(&f.session);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_fork_without_recording_leaves_state(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    add_turn(&f.session, "second", "r2");
    f.state.session_log = NULL; /* recording off/unavailable: nothing to preserve */
    size_t items_before = f.session.n_items;

    struct history_mutation_call c = {.state = &f.state, .turn_index = 1};
    char *out = capture_stdout(do_fork, &c);

    /* Refused, conversation fully intact. */
    EXPECT(strstr(out, "session recording") != NULL);
    EXPECT(f.session.n_items == items_before);
    EXPECT(f.state.pending_recall == NULL);
    EXPECT(f.state.session_log == NULL);

    free(out);
    agent_session_free(&f.session);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

/* A fork branches from a prefix that may predate the run's current settings
 * (a /model or /preset switch since), so the branch has to carry what the run
 * is actually on — otherwise resuming the fork would snap back to the
 * prefix's older selection. */
static void test_fork_records_live_selection(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    add_turn(&f.session, "second", "r2");

    /* The source file records an older selection than the live one
     * (prov-x · model-a, from the fixture). */
    f.state.session_log = session_log_open("old-prov", "old-model", NULL, "low", "old-stance");
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    char *orig = xstrdup(session_log_path(f.state.session_log));

    struct history_mutation_call c = {.state = &f.state, .turn_index = 1};
    char *out = capture_stdout(do_fork, &c);
    char *branch = xstrdup(session_log_path(f.state.session_log));

    /* Branching alone changes nothing on disk — the prefix is still all the
     * branch has produced. */
    struct session_meta fm;
    EXPECT(session_read_meta(branch, &fm) == 0);
    EXPECT_STR_EQ(fm.provider, "old-prov");
    session_meta_free(&fm);

    /* The next turn is the run's, so it carries the run's selection. */
    add_turn(&f.session, "third", "r3");
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    EXPECT(session_read_meta(branch, &fm) == 0);
    EXPECT_STR_EQ(fm.provider, "prov-x");
    EXPECT_STR_EQ(fm.model, "model-a");
    EXPECT(fm.preset == NULL); /* no stance is active in this run */
    session_meta_free(&fm);
    free(branch);

    /* The source branch is untouched — still resumable as it was. */
    EXPECT(session_read_meta(orig, &fm) == 0);
    EXPECT_STR_EQ(fm.provider, "old-prov");
    EXPECT_STR_EQ(fm.model, "old-model");
    EXPECT_STR_EQ(fm.preset, "old-stance");
    session_meta_free(&fm);

    free(out);
    free(orig);
    free(f.state.pending_recall);
    session_log_close(f.state.session_log);
    fixture_free(&f);
}

/* Session metadata is read back by a resume and handed to provider_find, so
 * it has to carry the resolvable provider id — not the display name, which
 * a configured display_name can set to something that resolves to nothing. */
static void test_session_records_provider_id(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f); /* clears the env, so pin the id after it */
    setenv("HAX_PROVIDER", "prov-id", 1);
    f.provider.name = "Display Name"; /* what a display_name override leaves behind */
    add_turn(&f.session, "first", "r1");

    f.state.session_log =
        session_log_open(agent_provider_id(&f.provider), f.session.model, NULL, NULL, NULL);
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    char *path = xstrdup(session_log_path(f.state.session_log));

    struct session_meta m;
    EXPECT(session_read_meta(path, &m) == 0);
    EXPECT_STR_EQ(m.provider, "prov-id");
    session_meta_free(&m);

    free(path);
    session_log_close(f.state.session_log);
    fixture_free(&f);
    unsetenv("HAX_PROVIDER");
}

/* A mid-session switch has to reach the session file, or a later resume would
 * restore what the conversation started on rather than what it ended on. */
static void test_apply_settings_records_switch(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");

    f.state.session_log = session_log_open("prov-x", "model-a", NULL, NULL, NULL);
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items,
                       f.session.n_items); /* materializes it */
    char *path = xstrdup(session_log_path(f.state.session_log));

    setenv("HAX_MODEL", "model-b", 1);
    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.result == 0);

    /* A switch the user hasn't used yet stays out of the file. */
    struct session_meta m;
    EXPECT(session_read_meta(path, &m) == 0);
    EXPECT_STR_EQ(m.model, "model-a");
    session_meta_free(&m);

    /* The turn that follows it is recorded under it. */
    add_turn(&f.session, "second", "r2");
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    EXPECT(session_read_meta(path, &m) == 0);
    EXPECT_STR_EQ(m.provider, "prov-x");
    EXPECT_STR_EQ(m.model, "model-b"); /* the header still says model-a */
    session_meta_free(&m);

    free(out);
    free(path);
    session_log_close(f.state.session_log);
    fixture_free(&f);
}

static void test_undo_intact_when_truncate_fails(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.session, "first", "r1");
    add_turn(&f.session, "second", "r2");
    add_turn(&f.session, "third", "r3");

    f.state.session_log = session_log_open("prov-x", "model-a", NULL, NULL, NULL);
    EXPECT(f.state.session_log != NULL);
    session_log_append(f.state.session_log, f.session.items, f.session.n_items);
    size_t items_before = f.session.n_items;

    /* Make the on-disk truncation fail: unlink the file so scan_turn_offset's
     * reopen can't find it. agent_undo must bail before touching memory. */
    EXPECT(unlink(session_log_path(f.state.session_log)) == 0);

    struct history_mutation_call c = {.state = &f.state, .turn_index = 1};
    char *out = capture_stdout(do_undo, &c);

    EXPECT(strstr(out, "could not truncate") != NULL);
    EXPECT(f.session.n_items == items_before); /* history untouched */
    EXPECT(f.state.pending_recall == NULL);

    free(out);
    session_log_close(f.state.session_log);
    agent_session_free(&f.session);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

int main(void)
{
    test_apply_settings_empty_reprints_banner();
    test_apply_settings_nonempty_prints_marker();
    test_apply_settings_quiet_prints_nothing();
    test_apply_settings_no_model_fails_intact();
    test_apply_settings_failed_provider_change_keeps_old();
    test_apply_settings_refreshes_on_model_or_provider_change();
    test_resync_effort_follows_late_metadata();
    test_apply_settings_keeps_stamped_spend();
    test_new_conversation_resets_everything();
    test_continue_marker_is_not_a_user_turn();
    test_undo_reverts_history_and_file();
    test_undo_with_continuation_cuts_disk_and_memory_alike();
    test_fork_branches_and_switches_log();
    test_fork_at_tip_clones_whole();
    test_fork_without_recording_leaves_state();
    test_fork_records_live_selection();
    test_apply_settings_records_switch();
    test_session_records_provider_id();
    test_undo_intact_when_truncate_fails();
    T_REPORT();
}
