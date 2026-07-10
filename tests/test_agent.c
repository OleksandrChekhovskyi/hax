/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "harness.h"
#include "provider.h"
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
 * so resolve_reasoning_effort is deterministically NULL. tlog/slog stay
 * NULL — both log layers are NULL-safe by contract. */
struct fixture {
    struct provider p;
    struct agent_session sess;
    struct render_ctx r;
    struct agent_state st;
    int rc;
};

static void fixture_init(struct fixture *f)
{
    setenv("HAX_MODEL", "model-a", 1);
    setenv("HAX_SYSTEM_PROMPT", "sys", 1);
    setenv("HAX_NO_ENV", "1", 1);
    setenv("HAX_NO_AGENTS_MD", "1", 1);
    unsetenv("HAX_REASONING_EFFORT");

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
    f->rc = agent_apply_settings(&f->st);
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
static char refresh_last_model[64];

static void counting_refresh_context(struct provider *p, const char *model)
{
    (void)p;
    refresh_calls++;
    snprintf(refresh_last_model, sizeof(refresh_last_model), "%s", model ? model : "");
}

static void test_apply_settings_refresh_only_on_model_change(void)
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

/* agent_print_banner only reads name/model/model_label/reasoning_effort,
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
        .reasoning_effort = "high",
    };
    struct banner_call c = {.p = &p, .s = &s};
    char *out = capture_stdout(do_banner, &c);
    EXPECT(strstr(out, "prov-x · short-name · high") != NULL);
    EXPECT(strstr(out, "long-file-name") == NULL);
    free(out);
}

int main(void)
{
    test_apply_settings_empty_reprints_banner();
    test_apply_settings_nonempty_prints_marker();
    test_apply_settings_no_model_fails_intact();
    test_apply_settings_refresh_only_on_model_change();
    test_new_conversation_resets_everything();
    test_banner_no_provider_points_at_picker();
    test_banner_no_model_points_at_picker();
    test_banner_prefers_label_and_appends_effort();
    T_REPORT();
}
