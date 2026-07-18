/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "agent.h"
#include "agent_core.h"
#include "harness.h"
#include "provider.h"
#include "session.h"
#include "render/render_ctx.h"
#include "util.h"

/* Tests for agent.c's settings-change confirmation: agent_apply_settings
 * prints a fresh banner while the conversation is empty (the startup
 * banner right above would otherwise keep asserting the old settings)
 * and the dim "switched to" marker once history exists (a banner there
 * would falsely imply a reset).
 *
 * Unlike test_slash.c, this binary deliberately links the real agent.o
 * (hax_dep's lazy archive pulls its closure in), so no tool or banner
 * stubs: the real TOOL_* symbols come along via agent_core's TOOLS[]. */

/* Redirect stdout to a temp file so we can inspect what the call printed.
 * Same pattern as test_slash.c. Returns the captured bytes (caller frees)
 * and restores stdout. */
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

/* The fixture pins every input agent_apply_settings resolves from:
 * HAX_MODEL (env tier — shadows any real config/state.json on the
 * machine running the tests) and a provider without an effort ladder,
 * so resolve_effort is deterministically NULL. tlog/slog stay
 * NULL — both log layers are NULL-safe by contract. */
struct fixture {
    struct provider p;
    struct agent_session sess;
    struct render_ctx r;
    struct agent_state st;
    struct provider *candidate;
    int rc;
};

static void fixture_init(struct fixture *f)
{
    setenv("HAX_MODEL", "model-a", 1);
    setenv("HAX_SYSTEM_PROMPT", "sys", 1);
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    unsetenv("HAX_EFFORT");

    memset(f, 0, sizeof(*f));
    f->p.name = "prov-x";
    struct hax_opts opts = {0};
    EXPECT(agent_session_init(&f->sess, &f->p, &opts) == 0);

    /* Model the dispatcher's state at the point select.c calls apply:
     * the leading-gap separator has run, cursor on a blank line. */
    f->r.disp.trail = 2;
    f->st.sess = &f->sess;
    f->st.provider = &f->p;
    f->st.r = &f->r;
    f->candidate = &f->p;
}

static void fixture_free(struct fixture *f)
{
    agent_session_free(&f->sess);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void do_apply(void *user)
{
    struct fixture *f = user;
    f->rc = agent_apply_settings(&f->st, f->candidate);
}

static void test_apply_settings_empty_reprints_banner(void)
{
    struct fixture f;
    fixture_init(&f);
    EXPECT(f.sess.n_items == 0);

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == 0);
    /* The full two-row banner, carrying the new identity... */
    EXPECT(strstr(out, "hax") != NULL);
    EXPECT(strstr(out, "prov-x · model-a") != NULL);
    EXPECT(strstr(out, "ctrl-d quit") != NULL);
    /* ...instead of the mid-conversation marker. */
    EXPECT(strstr(out, "switched to") == NULL);
    /* The banner bypasses disp, so the branch must resync the trail to
     * the fresh line its raw output ended on — otherwise the pre-prompt
     * separator would trust the stale pre-banner value. */
    EXPECT(f.r.disp.trail == 1);

    free(out);
    fixture_free(&f);
}

static void test_apply_settings_nonempty_prints_marker(void)
{
    struct fixture f;
    fixture_init(&f);
    agent_session_add_user(&f.sess, "hello");
    EXPECT(f.sess.n_items > 0);

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == 0);
    EXPECT(strstr(out, "switched to prov-x · model-a") != NULL);
    EXPECT(strstr(out, "ctrl-d quit") == NULL);
    /* The marker drives disp itself, so its trailing newline is held
     * (deferred), not committed — no manual trail resync involved. */
    EXPECT(f.r.disp.trail == 0);
    EXPECT(f.r.disp.held == 1);

    free(out);
    fixture_free(&f);
}

static void test_apply_settings_no_model_fails_intact(void)
{
    struct fixture f;
    fixture_init(&f);
    agent_session_add_user(&f.sess, "hello");
    size_t items_before = f.sess.n_items;
    char *model_before = xstrdup(f.sess.model);

    /* Pull the model out from under the next resolve: no env value and no
     * provider default. reconfigure must fail without touching history or
     * the currently-applied model, and print no confirmation. (Its "no
     * model available" diagnostic goes to stderr and shows in the test
     * log — expected, not a failure.) */
    unsetenv("HAX_MODEL");
    f.p.default_model = NULL;

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == -1);
    EXPECT(f.sess.n_items == items_before);
    EXPECT_STR_EQ(f.sess.model, model_before);
    EXPECT(strstr(out, "switched to") == NULL);
    EXPECT(strstr(out, "ctrl-d quit") == NULL);

    free(out);
    free(model_before);
    fixture_free(&f);
}

/* ---------- agent_apply_settings: refresh_context gate ---------- */

static int refresh_calls;
static int provider_destroy_calls;
static char refresh_last_model[64];

static void counting_provider_destroy(struct provider *p)
{
    (void)p;
    provider_destroy_calls++;
}

static void counting_refresh_context(struct provider *p, const char *model)
{
    (void)p;
    refresh_calls++;
    snprintf(refresh_last_model, sizeof(refresh_last_model), "%s", model ? model : "");
}

static void test_apply_settings_failed_provider_change_keeps_old(void)
{
    struct fixture f;
    fixture_init(&f);
    unsetenv("HAX_MODEL");
    f.p.destroy = counting_provider_destroy;
    struct provider next = {.name = "prov-y"};
    f.candidate = &next;
    provider_destroy_calls = 0;

    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == -1);
    EXPECT(f.st.provider == &f.p);
    EXPECT(provider_destroy_calls == 0);
    EXPECT_STR_EQ(f.sess.provider_name, "prov-x");
    EXPECT(out[0] == '\0');

    free(out);
    fixture_free(&f);
}

static void test_apply_settings_refreshes_on_model_or_provider_change(void)
{
    struct fixture f;
    fixture_init(&f);
    f.p.refresh_context = counting_refresh_context;
    refresh_calls = 0;

    /* Same model re-applied (the /effort-tweak shape): the context probe
     * must not re-run — re-probing on every apply would add a needless
     * network round-trip and cancel/join churn. */
    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == 0);
    EXPECT(refresh_calls == 0);
    free(out);

    /* A real model change re-probes, with the new model. */
    setenv("HAX_MODEL", "model-b", 1);
    out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == 0);
    EXPECT(refresh_calls == 1);
    EXPECT_STR_EQ(refresh_last_model, "model-b");
    free(out);

    /* Provider identity is independently load-bearing: a fresh provider may
     * have skipped its constructor probe or probed a default model. Even when
     * the selected model string stays identical, refresh it once after the
     * ownership swap. */
    struct provider next = {
        .name = "prov-y",
        .refresh_context = counting_refresh_context,
    };
    f.p.destroy = counting_provider_destroy;
    f.candidate = &next;
    refresh_calls = 0;
    provider_destroy_calls = 0;
    out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == 0);
    EXPECT(f.st.provider == &next);
    EXPECT(provider_destroy_calls == 1);
    EXPECT(refresh_calls == 1);
    EXPECT_STR_EQ(refresh_last_model, "model-b");
    free(out);

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
    f.p.catalog_id = "prov";
    struct stream_usage u = {.input_tokens = 1000000,
                             .output_tokens = 1000000,
                             .cached_tokens = -1,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = -1};
    spend_account(&f.st.stats.spend, &u, "prov", "model-a");

    setenv("HAX_MODEL", "model-b", 1); /* model-a -> model-b */
    char *out = capture_stdout(do_apply, &f);
    EXPECT(f.rc == 0);
    int approx = 0;
    EXPECT(agent_session_spend(&f.st.stats, &approx) == 10.0); /* 1M*$2 + 1M*$8 per Mtok */
    EXPECT(approx == 1);
    free(out);

    /* A record whose stamp resolves nowhere (catalog fetch never landed,
     * unknown model) leaves the total at the reported subtotal, marked
     * approximate — it's missing real usage. */
    spend_account(&f.st.stats.spend, &u, "no-such-catalog-provider", "model-x");
    f.st.stats.spend.reported = 0.03;
    approx = 0;
    EXPECT(agent_session_spend(&f.st.stats, &approx) == 10.03);
    EXPECT(approx == 1);

    spend_free(&f.st.stats.spend);
    fixture_free(&f);
}

/* ---------- agent_new_conversation ---------- */

static void do_new_conversation(void *user)
{
    struct fixture *f = user;
    agent_new_conversation(&f->st);
}

static void test_new_conversation_resets_everything(void)
{
    struct fixture f;
    fixture_init(&f);

    /* Seed every per-conversation accumulator /new promises to clear. */
    agent_session_add_user(&f.sess, "hello");
    f.st.stats.turns = 3;
    f.st.stats.requests = 7;
    f.st.stats.input_tokens = 1000;
    f.st.stats.tool_calls = 2;

    char *out = capture_stdout(do_new_conversation, &f);
    EXPECT(f.sess.n_items == 0);
    struct session_stats zero = {0};
    EXPECT(memcmp(&f.st.stats, &zero, sizeof(zero)) == 0);
    /* The fresh start is announced with the same banner as startup. */
    EXPECT(strstr(out, "prov-x · model-a") != NULL);
    EXPECT(strstr(out, "ctrl-d quit") != NULL);

    free(out);
    fixture_free(&f);
}

/* ---------- agent_print_banner: fallback forms ---------- */

/* agent_print_banner only reads name/model/model_label/effort,
 * so hand-built structs suffice — no session init, no env. The asserts
 * pin the fallback logic (which hint, which label), not exact wording. */

struct banner_call {
    const struct provider *p;
    const struct agent_session *s;
};

static void do_banner(void *user)
{
    struct banner_call *c = user;
    agent_print_banner(c->p, c->s);
}

static void test_banner_no_provider_points_at_picker(void)
{
    struct banner_call c = {.p = NULL, .s = NULL};
    char *out = capture_stdout(do_banner, &c);
    EXPECT(strstr(out, "/provider") != NULL);
    EXPECT(strstr(out, "ctrl-d quit") != NULL);
    free(out);
}

static void test_banner_no_model_points_at_picker(void)
{
    struct provider p = {.name = "prov-x"};
    struct agent_session s = {0};
    struct banner_call c = {.p = &p, .s = &s};
    char *out = capture_stdout(do_banner, &c);
    EXPECT(strstr(out, "prov-x") != NULL);
    EXPECT(strstr(out, "/model") != NULL);
    free(out);
}

static void test_banner_prefers_label_and_appends_effort(void)
{
    struct provider p = {.name = "prov-x"};
    struct agent_session s = {
        .model = "/models/long-file-name.gguf",
        .model_label = "short-name",
        .effort = "high",
    };
    struct banner_call c = {.p = &p, .s = &s};
    char *out = capture_stdout(do_banner, &c);
    EXPECT(strstr(out, "prov-x · short-name · high") != NULL);
    EXPECT(strstr(out, "long-file-name") == NULL);
    free(out);
}

/* ---------- agent_undo / agent_fork: the real mutators ---------- */

/* These exercise the composition test_session.c can't reach: the in-memory
 * cut, the session-log swap, pending_recall capture, and failure atomicity,
 * all driven through the live agent.o. replay_user_turn no-ops off a tty (as
 * here), so the assertions pin state, not the replayed rule; ui_error prints
 * to stdout regardless, so the failure paths check the message too. */

static void add_turn(struct agent_session *s, const char *prompt, const char *reply)
{
    agent_session_add_user(s, prompt); /* boundary + user */
    items_append(&s->items, &s->n_items, &s->cap_items,
                 (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup(reply)});
}

/* Fresh, isolated per-cwd session tree, with recording enabled. */
static void set_state_dir(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
}

static size_t count_users(const struct item *items, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (items[i].kind == ITEM_USER_MESSAGE && !items[i].compact_seed)
            c++;
    return c;
}

static void free_items(struct item *items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        item_free(&items[i]);
    free(items);
}

struct mut_call {
    struct agent_state *st;
    size_t turn;
};

static void do_undo(void *user)
{
    struct mut_call *c = user;
    agent_undo(c->st, c->turn);
}

static void do_fork(void *user)
{
    struct mut_call *c = user;
    agent_fork(c->st, c->turn);
}

static void test_undo_reverts_history_and_file(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.sess, "first", "r1");
    add_turn(&f.sess, "second", "r2");
    add_turn(&f.sess, "third", "r3");

    f.st.slog = session_log_open("prov-x", "model-a", NULL);
    EXPECT(f.st.slog != NULL);
    session_log_append(f.st.slog, f.sess.items, f.sess.n_items);
    char *path = xstrdup(session_log_path(f.st.slog));
    EXPECT(agent_user_turn_count(&f.sess) == 3);

    /* Revert to before turn 1: turn 0 survives, turns 1 and 2 drop. */
    struct mut_call c = {.st = &f.st, .turn = 1};
    char *out = capture_stdout(do_undo, &c);

    EXPECT(agent_user_turn_count(&f.sess) == 1);
    EXPECT_STR_EQ(agent_user_turn_text(&f.sess, 0), "first");
    /* The discarded prompt is staged for editor recall. */
    EXPECT_STR_EQ(f.st.pending_recall, "second");

    /* The truncation reached disk: reloading shows only turn 0. */
    struct item *items;
    size_t n;
    EXPECT(session_load(path, &items, &n, NULL) == 0);
    EXPECT(count_users(items, n) == 1);
    free_items(items, n);

    free(out);
    free(path);
    free(f.st.pending_recall);
    session_log_close(f.st.slog);
    agent_session_free(&f.sess);
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
    add_turn(&f.sess, "first", "r1");
    add_turn(&f.sess, "second", "r2");
    add_turn(&f.sess, "third", "r3");

    f.st.slog = session_log_open("prov-x", "model-a", NULL);
    EXPECT(f.st.slog != NULL);
    session_log_append(f.st.slog, f.sess.items, f.sess.n_items);
    char *orig = xstrdup(session_log_path(f.st.slog));

    struct mut_call c = {.st = &f.st, .turn = 1};
    char *out = capture_stdout(do_fork, &c);

    /* History cut to the branch point, discarded prompt staged. */
    EXPECT(agent_user_turn_count(&f.sess) == 1);
    EXPECT_STR_EQ(f.st.pending_recall, "second");

    /* The live log moved to a new file... */
    const char *newpath = session_log_path(f.st.slog);
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
    free(f.st.pending_recall);
    session_log_close(f.st.slog);
    agent_session_free(&f.sess);
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
    add_turn(&f.sess, "first", "r1");
    add_turn(&f.sess, "second", "r2");

    f.st.slog = session_log_open("prov-x", "model-a", NULL);
    EXPECT(f.st.slog != NULL);
    session_log_append(f.st.slog, f.sess.items, f.sess.n_items);
    char *orig = xstrdup(session_log_path(f.st.slog));
    size_t items_before = f.sess.n_items;

    /* turn == count: clone at the tip, nothing discarded. */
    struct mut_call c = {.st = &f.st, .turn = 2};
    char *out = capture_stdout(do_fork, &c);

    EXPECT(f.sess.n_items == items_before);
    EXPECT(f.st.pending_recall == NULL); /* no prompt discarded */

    const char *newpath = session_log_path(f.st.slog);
    EXPECT(strcmp(newpath, orig) != 0);
    struct item *items;
    size_t n;
    EXPECT(session_load(newpath, &items, &n, NULL) == 0);
    EXPECT(count_users(items, n) == 2); /* full clone */
    free_items(items, n);

    free(out);
    free(orig);
    session_log_close(f.st.slog);
    agent_session_free(&f.sess);
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
    add_turn(&f.sess, "first", "r1");
    add_turn(&f.sess, "second", "r2");
    f.st.slog = NULL; /* recording off/unavailable: nothing to preserve */
    size_t items_before = f.sess.n_items;

    struct mut_call c = {.st = &f.st, .turn = 1};
    char *out = capture_stdout(do_fork, &c);

    /* Refused, conversation fully intact. */
    EXPECT(strstr(out, "session recording") != NULL);
    EXPECT(f.sess.n_items == items_before);
    EXPECT(f.st.pending_recall == NULL);
    EXPECT(f.st.slog == NULL);

    free(out);
    agent_session_free(&f.sess);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

static void test_undo_intact_when_truncate_fails(void)
{
    set_state_dir();
    struct fixture f;
    fixture_init(&f);
    add_turn(&f.sess, "first", "r1");
    add_turn(&f.sess, "second", "r2");
    add_turn(&f.sess, "third", "r3");

    f.st.slog = session_log_open("prov-x", "model-a", NULL);
    EXPECT(f.st.slog != NULL);
    session_log_append(f.st.slog, f.sess.items, f.sess.n_items);
    size_t items_before = f.sess.n_items;

    /* Make the on-disk truncation fail: unlink the file so scan_turn_offset's
     * reopen can't find it. agent_undo must bail before touching memory. */
    EXPECT(unlink(session_log_path(f.st.slog)) == 0);

    struct mut_call c = {.st = &f.st, .turn = 1};
    char *out = capture_stdout(do_undo, &c);

    EXPECT(strstr(out, "could not truncate") != NULL);
    EXPECT(f.sess.n_items == items_before); /* history untouched */
    EXPECT(f.st.pending_recall == NULL);

    free(out);
    session_log_close(f.st.slog);
    agent_session_free(&f.sess);
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_NO_ENV");
    unsetenv("HAX_NO_AGENTS_MD");
}

int main(void)
{
    test_apply_settings_empty_reprints_banner();
    test_apply_settings_nonempty_prints_marker();
    test_apply_settings_no_model_fails_intact();
    test_apply_settings_failed_provider_change_keeps_old();
    test_apply_settings_refreshes_on_model_or_provider_change();
    test_apply_settings_keeps_stamped_spend();
    test_new_conversation_resets_everything();
    test_banner_no_provider_points_at_picker();
    test_banner_no_model_points_at_picker();
    test_banner_prefers_label_and_appends_effort();
    test_undo_reverts_history_and_file();
    test_fork_branches_and_switches_log();
    test_fork_at_tip_clones_whole();
    test_fork_without_recording_leaves_state();
    test_undo_intact_when_truncate_fails();
    T_REPORT();
}
