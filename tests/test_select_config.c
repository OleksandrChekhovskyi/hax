/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "agent_core.h"
#include "config.h"
#include "diag.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "select.h"
#include "xalloc.h"
#include "render/render_ctx.h"
#include "terminal/picker.h"
#include "transport/http.h"

/* select.c reaches into agent.c for these; stub them so the test links without pulling the whole
 * REPL graph. */
static int g_apply_result;
static int g_apply_calls;
static int g_apply_replace_model;

int agent_apply_settings(struct agent_state *state, struct provider *provider, int announce)
{
    (void)provider;
    (void)announce;
    g_apply_calls++;
    if (g_apply_replace_model) {
        free(state->session->model);
        state->session->model = xstrdup(config_str("model"));
    }
    return g_apply_result;
}
void agent_display_refresh(struct agent_state *state)
{
    (void)state;
}

/* Scripted picker: each call selects the row whose label matches the next
 * entry (NULL or past the end = cancel). One script drives both the outer
 * config list and the inner choice list, in call order. */
static const char *g_picks[4];
static int g_pick_count;
static int g_pick_index;
static char g_picked_detail[256];
static char g_picked_description[256];

long picker_run(const struct picker_opts *options)
{
    if (g_pick_index >= g_pick_count || !g_picks[g_pick_index])
        return -1;
    const char *label = g_picks[g_pick_index++];
    for (size_t i = 0; i < options->item_count; i++) {
        if (!options->items[i].label || strcmp(options->items[i].label, label) != 0)
            continue;
        snprintf(g_picked_detail, sizeof(g_picked_detail), "%s",
                 options->items[i].detail ? options->items[i].detail : "");
        snprintf(g_picked_description, sizeof(g_picked_description), "%s",
                 options->items[i].description ? options->items[i].description : "");
        return (long)i;
    }
    return -1;
}

static void script_picks(const char *a, const char *b)
{
    g_picks[0] = a;
    g_picks[1] = b;
    g_pick_count = (a ? 1 : 0) + (b ? 1 : 0);
    g_pick_index = 0;
    g_picked_detail[0] = '\0';
    g_picked_description[0] = '\0';
}

/* Fresh tiers and no stray env for the keys under test. */
static void reset(void)
{
    config_free();
    const char *vars[] = {"HAX_MARKDOWN",      "HAX_THEME",    "HAX_SORT_MODELS",
                          "HAX_DISPLAY_WIDTH", "HAX_PROVIDER", "HAX_MODEL",
                          "HAX_EFFORT",        "HAX_PRESET",   "HAX_OPENAI_BASE_URL"};
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
        unsetenv(vars[i]);
    script_picks(NULL, NULL);
    g_apply_result = 0;
    g_apply_calls = 0;
    g_apply_replace_model = 0;
}

static struct render_ctx g_render;
static struct agent_state g_state;

static struct agent_state *fresh_state(void)
{
    memset(&g_render, 0, sizeof g_render);
    memset(&g_state, 0, sizeof g_state);
    g_state.render = &g_render;
    return &g_state;
}

/* Run select_config, returning everything it printed (caller frees). */
static char *run(struct agent_state *state, const char *arg)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    EXPECT(saved >= 0);
    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    EXPECT(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

    select_config(state, arg);

    fflush(stdout);
    EXPECT(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);

    fseek(tmp, 0, SEEK_END);
    long sz = ftell(tmp);
    rewind(tmp);
    char *buf = xmalloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, tmp);
    buf[got] = '\0';
    fclose(tmp);
    return buf;
}

static void test_unknown_setting(void)
{
    reset();
    struct agent_state *state = fresh_state();
    char *out = run(state, "nonesuch value");
    EXPECT(strstr(out, "unknown setting") != NULL);
    free(out);
}

static void test_readonly_paths(void)
{
    reset();
    struct agent_state *state = fresh_state();
    /* A setting with a dedicated command points at it. */
    char *out = run(state, "provider mock");
    EXPECT(strstr(out, "/provider") != NULL);
    EXPECT_STR_EQ(config_source("provider"), "default"); /* not committed */
    free(out);

    /* One without falls back to its env var. */
    out = run(state, "providers.openai-compatible.base_url http://x");
    EXPECT(strstr(out, "HAX_OPENAI_BASE_URL") != NULL);
    free(out);
}

static void test_set_and_default(void)
{
    reset();
    struct agent_state *state = fresh_state();
    char *out = run(state, "markdown off");
    EXPECT_STR_EQ(config_source("markdown"), "run");
    EXPECT(config_bool("markdown") == 0);
    EXPECT(strstr(out, "markdown = off") != NULL);
    EXPECT(strstr(out, "run") != NULL);
    free(out);

    /* "default" clears the override so lower tiers resolve again. */
    out = run(state, "markdown default");
    EXPECT_STR_EQ(config_source("markdown"), "default");
    EXPECT(config_bool("markdown") == 1); /* registry default */
    free(out);
}

static void test_invalid_value(void)
{
    reset();
    struct agent_state *state = fresh_state();
    char *out = run(state, "markdown banana");
    EXPECT(strstr(out, "invalid value") != NULL);
    EXPECT_STR_EQ(config_source("markdown"), "default"); /* rejected, not stored */
    free(out);
}

static void test_canonicalization(void)
{
    reset();
    struct agent_state *state = fresh_state();
    /* A strict enum stores its canonical spelling, not the typed case. */
    char *out = run(state, "theme LIGHT");
    EXPECT_STR_EQ(config_str("theme"), "light");
    EXPECT(strstr(out, "theme = light") != NULL);
    free(out);
}

static void test_tristate_alias_normalizes(void)
{
    reset();
    struct agent_state *state = fresh_state();
    /* Tri-state accepts a bool alias and the display normalizes it to on/off. */
    char *out = run(state, "sort_models 1");
    EXPECT(config_bool_or("sort_models", 0) == 1);
    EXPECT(strstr(out, "sort_models = on") != NULL);
    free(out);
}

static void test_show_current(void)
{
    reset();
    struct agent_state *state = fresh_state();
    /* No value: a runtime setting shows its current value (no error). */
    char *out = run(state, "markdown");
    EXPECT(strstr(out, "markdown = ") != NULL);
    EXPECT(strstr(out, "invalid") == NULL);
    free(out);
}

static void test_picker_preseeds_mixed_value(void)
{
    reset();
    struct agent_state *state = fresh_state();

    /* An existing typed value is preserved when handing off from the choice
     * picker to the editor. */
    config_set_override("display_width", "120");
    script_picks("display_width", "exact value...");
    char *out = run(state, NULL);
    EXPECT_STR_EQ(g_picked_detail, "");
    EXPECT_STR_EQ(g_picked_description, "Enter an exact value such as 100");
    EXPECT_STR_EQ(state->pending_preseed, "/config display_width 120");
    free(state->pending_preseed);
    state->pending_preseed = NULL;
    free(out);

    /* A symbolic current value uses the registry's concrete example. */
    reset();
    state = fresh_state();
    script_picks("display_width", "exact value...");
    out = run(state, NULL);
    EXPECT_STR_EQ(state->pending_preseed, "/config display_width 100");
    free(state->pending_preseed);
    state->pending_preseed = NULL;
    free(out);
}

static void test_picker_commits_choice(void)
{
    reset();
    struct agent_state *state = fresh_state();
    /* Outer list picks the setting, inner list picks the value. */
    script_picks("markdown", "off");
    char *out = run(state, NULL);
    EXPECT_STR_EQ(config_source("markdown"), "run");
    EXPECT(config_bool("markdown") == 0);
    free(out);

    reset();
    state = fresh_state();
    config_set_override("markdown", "off");
    script_picks("markdown", "default");
    out = run(state, NULL);
    EXPECT_STR_EQ(g_picked_detail, "");
    EXPECT_STR_EQ(g_picked_description, "Clear the runtime override and use the environment, saved "
                                        "configuration, or built-in default");
    EXPECT_STR_EQ(config_source("markdown"), "default");
    free(out);

    reset();
    state = fresh_state();
    script_picks("display_width", "terminal");
    out = run(state, NULL);
    EXPECT_STR_EQ(config_str("display_width"), "terminal");
    free(out);
}

static size_t test_list_efforts(struct provider *provider, const char *const **efforts)
{
    (void)provider;
    static const char *const levels[] = {"low", "high"};
    *efforts = levels;
    return sizeof(levels) / sizeof(levels[0]);
}

static int test_list_models(struct provider *provider, struct model_info **models,
                            size_t *model_count, char **error, http_tick_cb tick, void *tick_user)
{
    (void)provider;
    (void)error;
    (void)tick;
    (void)tick_user;
    *model_count = 2;
    *models = xcalloc(*model_count, sizeof(**models));
    model_info_init(&(*models)[0]);
    model_info_init(&(*models)[1]);
    (*models)[0].id = xstrdup("old");
    (*models)[0].context = 100000;
    (*models)[1].id = xstrdup("new");
    (*models)[1].context = 200000;
    return 0;
}

static void test_effort_apply_failure_restores_overrides(void)
{
    reset();
    struct agent_state *state = fresh_state();
    struct agent_session session = {.model = "model", .effort = "low"};
    struct provider provider = {.name = "test", .list_efforts = test_list_efforts};
    state->session = &session;
    state->provider = &provider;
    config_set_override("provider", "test");
    config_set_override("effort", "low");
    script_picks("high", NULL);
    g_apply_result = -1;

    select_effort(state);

    EXPECT(g_apply_calls == 1);
    EXPECT_STR_EQ(config_str("provider"), "test");
    EXPECT_STR_EQ(config_str("effort"), "low");
}

static void test_effort_persists_after_reconfiguration(void)
{
    reset();
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    struct agent_state *state = fresh_state();
    struct agent_session session = {.model = xstrdup("model"), .effort = xstrdup("low")};
    struct provider provider = {.name = "test", .list_efforts = test_list_efforts};
    state->session = &session;
    state->provider = &provider;
    config_set_override("provider", "test");
    config_set_override("model", "model");
    config_set_override("effort", "low");
    config_set_override("preset", "work");
    script_picks("high", NULL);
    g_apply_replace_model = 1;

    select_effort(state);

    EXPECT(g_apply_calls == 1);
    EXPECT_STR_EQ(config_str("effort"), "high");

    config_free();
    config_init();
    EXPECT_STR_EQ(config_str("provider"), "test");
    EXPECT_STR_EQ(config_str("model"), "model");
    EXPECT_STR_EQ(config_str("effort"), "high");
    EXPECT(config_str("preset") == NULL);

    free(session.model);
    free(session.effort);
    config_free();
    unsetenv("XDG_STATE_HOME");
}

static void test_model_apply_failure_restores_selection(void)
{
    reset();
    struct agent_state *state = fresh_state();
    struct agent_session session = {.model = "old"};
    struct provider provider = {
        .name = "test",
        .model_discovered = 1,
        .list_models = test_list_models,
    };
    state->session = &session;
    state->provider = &provider;
    config_set_override("provider", "test");
    config_set_override("model", "old");
    struct model_info old_metadata;
    model_info_init(&old_metadata);
    old_metadata.id = "old";
    old_metadata.context = 100000;
    model_meta_store(&provider, &old_metadata);
    script_picks("new", NULL);
    g_apply_result = -1;

    select_model(state);

    EXPECT(g_apply_calls == 1);
    EXPECT_STR_EQ(config_str("model"), "old");
    EXPECT(provider.model_discovered == 1);
    struct model_info restored_metadata;
    EXPECT(model_meta_snapshot(&provider, &restored_metadata) == 1);
    EXPECT_STR_EQ(restored_metadata.id, "old");
    EXPECT(restored_metadata.context == 100000);
    model_info_clear(&restored_metadata);
    model_meta_release(&provider);
}

/* Resuming a session recorded under a former provider id with an otherwise unchanged
 * selection takes the fast path: no reconstruction, no diagnostics, no run override. */
static void test_restore_session_former_id_fast_path(void)
{
    reset();
    struct agent_state *state = fresh_state();
    struct provider live = {.name = "llama.cpp", .id = "llamacpp"};
    struct agent_session session = {0};
    session.model = xstrdup("m1");
    state->provider = &live;
    state->session = &session;

    unsigned long diagnostics_before = hax_diag_sequence();
    select_restore_session(state, "llama.cpp", "m1", NULL, NULL);
    EXPECT(g_apply_calls == 0);
    EXPECT(hax_diag_sequence() == diagnostics_before);
    EXPECT_STR_EQ(config_source("provider"), "default");
    free(session.model);
}

int main(void)
{
    test_unknown_setting();
    test_readonly_paths();
    test_set_and_default();
    test_invalid_value();
    test_canonicalization();
    test_tristate_alias_normalizes();
    test_show_current();
    test_picker_preseeds_mixed_value();
    test_picker_commits_choice();
    test_effort_apply_failure_restores_overrides();
    test_effort_persists_after_reconfiguration();
    test_model_apply_failure_restores_selection();
    test_restore_session_former_id_fast_path();
    T_REPORT();
}
