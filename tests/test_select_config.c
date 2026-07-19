/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "config.h"
#include "harness.h"
#include "render/render_ctx.h"
#include "select.h"
#include "terminal/picker.h"
#include "util.h"

/* select.c reaches into agent.c for these; stub them so the test links
 * select.c without pulling the whole REPL graph (input, spinner, disp
 * pipeline). Neither has an observable effect on the /config paths here. */
int agent_apply_settings(struct agent_state *st, struct provider *p)
{
    (void)st;
    (void)p;
    return 0;
}
void agent_display_refresh(struct agent_state *st)
{
    (void)st;
}

/* Scripted picker: each call selects the row whose label matches the next
 * entry (NULL or past the end = cancel). One script drives both the outer
 * config list and the inner choice list, in call order. */
static const char *g_picks[4];
static int g_pick_n;
static int g_pick_i;

long picker_run(const struct picker_opts *opts)
{
    if (g_pick_i >= g_pick_n || !g_picks[g_pick_i])
        return -1;
    const char *want = g_picks[g_pick_i++];
    for (size_t i = 0; i < opts->n; i++)
        if (opts->items[i].label && strcmp(opts->items[i].label, want) == 0)
            return (long)i;
    return -1;
}

static void script_picks(const char *a, const char *b)
{
    g_picks[0] = a;
    g_picks[1] = b;
    g_pick_n = (a ? 1 : 0) + (b ? 1 : 0);
    g_pick_i = 0;
}

/* Fresh tiers and no stray env for the keys under test. */
static void reset(void)
{
    config_free();
    const char *vars[] = {"HAX_MARKDOWN",      "HAX_THEME",    "HAX_SORT_MODELS",
                          "HAX_DISPLAY_WIDTH", "HAX_PROVIDER", "HAX_OPENAI_BASE_URL"};
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
        unsetenv(vars[i]);
    script_picks(NULL, NULL);
}

static struct render_ctx g_rc;
static struct agent_state g_st;

static struct agent_state *fresh_state(void)
{
    memset(&g_rc, 0, sizeof g_rc);
    memset(&g_st, 0, sizeof g_st);
    g_st.r = &g_rc;
    return &g_st;
}

/* Run select_config, returning everything it printed (caller frees). */
static char *run(struct agent_state *st, const char *arg)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    EXPECT(saved >= 0);
    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    EXPECT(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

    select_config(st, arg);

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
    struct agent_state *st = fresh_state();
    char *out = run(st, "nonesuch value");
    EXPECT(strstr(out, "unknown setting") != NULL);
    free(out);
}

static void test_readonly_paths(void)
{
    reset();
    struct agent_state *st = fresh_state();
    /* A setting with a dedicated command points at it. */
    char *out = run(st, "provider mock");
    EXPECT(strstr(out, "/provider") != NULL);
    EXPECT_STR_EQ(config_source("provider"), "default"); /* not committed */
    free(out);

    /* One without falls back to its env var. */
    out = run(st, "openai.base_url http://x");
    EXPECT(strstr(out, "HAX_OPENAI_BASE_URL") != NULL);
    free(out);
}

static void test_set_and_default(void)
{
    reset();
    struct agent_state *st = fresh_state();
    char *out = run(st, "markdown off");
    EXPECT_STR_EQ(config_source("markdown"), "session");
    EXPECT(config_bool("markdown") == 0);
    EXPECT(strstr(out, "markdown = off") != NULL);
    EXPECT(strstr(out, "session") != NULL);
    free(out);

    /* "default" clears the override so lower tiers resolve again. */
    out = run(st, "markdown default");
    EXPECT_STR_EQ(config_source("markdown"), "default");
    EXPECT(config_bool("markdown") == 1); /* registry default */
    free(out);
}

static void test_invalid_value(void)
{
    reset();
    struct agent_state *st = fresh_state();
    char *out = run(st, "markdown banana");
    EXPECT(strstr(out, "invalid value") != NULL);
    EXPECT_STR_EQ(config_source("markdown"), "default"); /* rejected, not stored */
    free(out);
}

static void test_canonicalization(void)
{
    reset();
    struct agent_state *st = fresh_state();
    /* A strict enum stores its canonical spelling, not the typed case. */
    char *out = run(st, "theme LIGHT");
    EXPECT_STR_EQ(config_str("theme"), "light");
    EXPECT(strstr(out, "theme = light") != NULL);
    free(out);
}

static void test_tristate_alias_normalizes(void)
{
    reset();
    struct agent_state *st = fresh_state();
    /* Tri-state accepts a bool alias and the display normalizes it to on/off. */
    char *out = run(st, "sort_models 1");
    EXPECT(config_bool_or("sort_models", 0) == 1);
    EXPECT(strstr(out, "sort_models = on") != NULL);
    free(out);
}

static void test_show_current(void)
{
    reset();
    struct agent_state *st = fresh_state();
    /* No value: a runtime setting shows its current value (no error). */
    char *out = run(st, "markdown");
    EXPECT(strstr(out, "markdown = ") != NULL);
    EXPECT(strstr(out, "invalid") == NULL);
    free(out);
}

static void test_picker_preseeds_freeform(void)
{
    reset();
    struct agent_state *st = fresh_state();
    config_set_override("display_width", "120");
    script_picks("display_width", NULL);
    char *out = run(st, NULL);
    EXPECT_STR_EQ(st->pending_preseed, "/config display_width 120");
    free(st->pending_preseed);
    st->pending_preseed = NULL;
    free(out);
}

static void test_picker_commits_choice(void)
{
    reset();
    struct agent_state *st = fresh_state();
    /* Outer list picks the setting, inner list picks the value. */
    script_picks("markdown", "off");
    char *out = run(st, NULL);
    EXPECT_STR_EQ(config_source("markdown"), "session");
    EXPECT(config_bool("markdown") == 0);
    free(out);
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
    test_picker_preseeds_freeform();
    test_picker_commits_choice();
    T_REPORT();
}
