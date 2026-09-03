/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "diag.h"
#include "harness.h"
#include "xalloc.h"
#include "system/fs.h"

/* Isolate resolution tests from the developer or CI environment. */
static void clear_env(void)
{
    size_t count = 0;
    const struct config_setting *settings = config_settings(&count);
    for (size_t i = 0; i < count; i++)
        unsetenv(settings[i].env_var);
}

/* Write a config fixture for tests that exercise the file-backed API. */
static void write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    EXPECT(fp != NULL);
    if (!fp)
        return;
    EXPECT(fputs(text, fp) >= 0);
    EXPECT(fclose(fp) == 0);
}

static void test_load_validation(void)
{
    clear_env();
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_str("model") == NULL);
    EXPECT(config_load("") == 0);
    /* Malformed JSON and a valid-but-non-object root both fail and leave
     * the file tier empty. */
    EXPECT(config_load("{ not json") == -1);
    EXPECT(config_load("[1, 2, 3]") == -1);
    EXPECT(config_load("\"a string\"") == -1);
    EXPECT(config_str("model") == NULL);
}

static void test_nested_and_flat(void)
{
    clear_env();
    /* Nested objects are the friendly form. */
    EXPECT(config_load("{\"providers\": {\"openai-compatible\": {\"base_url\": \"nested\"}}}") ==
           0);
    EXPECT_STR_EQ(config_str("providers.openai-compatible.base_url"), "nested");
    /* A flat dotted key is accepted too. */
    EXPECT(config_load("{\"providers.openai-compatible.base_url\": \"flat\"}") == 0);
    EXPECT_STR_EQ(config_str("providers.openai-compatible.base_url"), "flat");
}

static void test_scalar_normalization(void)
{
    clear_env();
    /* Numbers and bools read as strings, so the typed getters work whether
     * the file wrote 64000 or "64000", true or "1". */
    EXPECT(config_load("{\"context_limit\": 64000, \"display_width\": 120,"
                       " \"show_reasoning\": true}") == 0);
    EXPECT(config_tokens("context_limit") == 64000);
    EXPECT(config_int("display_width") == 120);
    EXPECT(config_bool("show_reasoning") == 1);
    EXPECT(config_load("{\"show_reasoning\": false}") == 0);
    EXPECT(config_bool("show_reasoning") == 0);
}

static void test_typed_getters(void)
{
    clear_env();
    EXPECT(config_load("{\"context_limit\": \"64k\", \"display_width\": \"100\","
                       " \"show_reasoning\": \"0\"}") == 0);
    EXPECT(config_tokens("context_limit") == 64000); /* token counts use decimal suffixes */
    EXPECT(config_int("display_width") == 100);
    EXPECT(config_bool("show_reasoning") == 0); /* explicit "0" is false */
    /* Unset typed reads are type-zero. */
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_int("display_width") == 0);
    EXPECT(config_tokens("context_limit") == 0);
    EXPECT(config_bool("show_reasoning") == 0);
}

static void test_registry_default(void)
{
    clear_env();
    config_load(NULL);
    /* llamacpp.port has a fixed default in the registry. */
    EXPECT_STR_EQ(config_str("providers.llamacpp.port"), "8080");
    /* File overrides the default. */
    EXPECT(config_load("{\"providers\": {\"llamacpp\": {\"port\": \"9090\"}}}") == 0);
    EXPECT_STR_EQ(config_str("providers.llamacpp.port"), "9090");
    /* Env overrides both. */
    setenv("HAX_LLAMACPP_PORT", "7070", 1);
    EXPECT_STR_EQ(config_str("providers.llamacpp.port"), "7070");
    unsetenv("HAX_LLAMACPP_PORT");
}

static void test_default_on_unset_and_invalid(void)
{
    clear_env();
    /* Unset reads the registry default through the typed getters. */
    config_load(NULL);
    EXPECT(config_duration_ms("bash.timeout") == 120 * 1000);
    EXPECT(config_duration_ms("bash.timeout_grace") == 2 * 1000);
    EXPECT(config_size("tool_output_cap") == 50 * 1024);
    EXPECT(config_int("http.max_retries") == 4);
    /* A set-but-unparseable value also falls back to the registry
     * default, not to zero — a typo'd timeout must not disable it. */
    EXPECT(config_load("{\"bash\": {\"timeout\": \"soon\"}, \"tool_output_cap\": \"big\","
                       " \"http\": {\"max_retries\": \"lots\"}}") == 0);
    EXPECT(config_duration_ms("bash.timeout") == 120 * 1000);
    EXPECT(config_size("tool_output_cap") == 50 * 1024);
    EXPECT(config_int("http.max_retries") == 4);
    /* An explicit duration of 0 is a valid parse ("0 disables"), not a
     * fallback; an explicit size of 0 reads as invalid (the cap can't
     * sensibly be zero) and falls back. */
    EXPECT(config_load("{\"bash\": {\"timeout\": \"0\"}, \"tool_output_cap\": 0}") == 0);
    EXPECT(config_duration_ms("bash.timeout") == 0);
    EXPECT(config_size("tool_output_cap") == 50 * 1024);
    /* Negative ints read as invalid too (counts/widths), not honored. */
    EXPECT(config_load("{\"http\": {\"max_retries\": \"-1\"}}") == 0);
    EXPECT(config_int("http.max_retries") == 4);
    /* Bools are explicit in both directions — an unrecognized spelling
     * is invalid → default, never silently truthy (most bool settings
     * are no_* switches, where accidental-true disables something). */
    EXPECT(config_load("{\"show_reasoning\": \"yes\", \"no_session\": \"banana\"}") == 0);
    EXPECT(config_bool("show_reasoning") == 1);
    EXPECT(config_bool("no_session") == 0);
    /* A bool with a registry default ("1" for markdown) reads as that
     * default when unset or unrecognized — never as off-by-typo. */
    EXPECT(config_load("{\"markdown\": \"treu\"}") == 0);
    EXPECT(config_bool("markdown") == 1);
    EXPECT(config_load("{\"markdown\": \"no\"}") == 0);
    EXPECT(config_bool("markdown") == 0);
    /* config_bool_or carries the caller's (per-preset) default through
     * unset, empty, and unrecognized values; a recognized value wins
     * either way. */
    EXPECT(config_load("{\"providers.openai-compatible.send_cache_key\": \"maybe\"}") == 0);
    EXPECT(config_bool_or("providers.openai-compatible.send_cache_key", 1) == 1);
    EXPECT(config_bool_or("providers.openai-compatible.send_cache_key", 0) == 0);
    EXPECT(config_load("{\"providers.openai-compatible.send_cache_key\": \"off\"}") == 0);
    EXPECT(config_bool_or("providers.openai-compatible.send_cache_key", 1) == 0);
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_bool_or("providers.openai-compatible.send_cache_key", 1) == 1); /* unset → def */
    /* No registry default → type-zero, as before. */
    EXPECT(config_load("{\"context_limit\": \"nope\"}") == 0);
    EXPECT(config_tokens("context_limit") == 0);
    /* config_default exposes the registry default tier directly. */
    EXPECT_STR_EQ(config_default("providers.llamacpp.port"), "8080");
    EXPECT(config_default("model") == NULL);
    EXPECT(config_default("no.such.key") == NULL);

    /* Registry bounds are enforced by the typed getters, so an out-of-range
     * value falls back to the default just like a parse failure — consumers
     * read the resolved value without re-clamping. */
    EXPECT(config_load("{\"compact\": {\"threshold\": \"50\"}}") == 0);
    EXPECT(config_int("compact.threshold") == 50); /* in [1,100] */
    EXPECT(config_load("{\"compact\": {\"threshold\": \"200\"}}") == 0);
    EXPECT(config_int("compact.threshold") == 85); /* above max → default */
    EXPECT(config_load("{\"compact\": {\"threshold\": \"0\"}}") == 0);
    EXPECT(config_int("compact.threshold") == 85); /* below min → default */
    EXPECT(config_load("{\"http\": {\"retry_base\": \"250ms\"}}") == 0);
    EXPECT(config_duration_ms("http.retry_base") == 250);
    EXPECT(config_load("{\"http\": {\"retry_base\": \"0\"}}") == 0);
    EXPECT(config_duration_ms("http.retry_base") == 1000); /* 0 < min → 1s default */
    EXPECT(config_load("{\"http\": {\"max_retries\": \"50\"}}") == 0);
    EXPECT(config_int("http.max_retries") == 50);
    EXPECT(config_load("{\"http\": {\"max_retries\": \"1000\"}}") == 0);
    EXPECT(config_int("http.max_retries") == 4); /* above max 100 → default */
}

static void test_env_wins_over_file(void)
{
    clear_env();
    config_load("{\"model\": \"from-file\"}");
    EXPECT_STR_EQ(config_str("model"), "from-file");
    setenv("HAX_MODEL", "from-env", 1);
    EXPECT_STR_EQ(config_str("model"), "from-env");
    /* Empty env is returned verbatim (so "" can mean "omit"). */
    setenv("HAX_MODEL", "", 1);
    const char *e = config_str("model");
    EXPECT(e != NULL && *e == '\0');
    unsetenv("HAX_MODEL");
    EXPECT_STR_EQ(config_str("model"), "from-file");
}

static void test_empty_means_unset(void)
{
    clear_env();
    /* For config_str_nonempty and the typed getters, an empty value is
     * "unset at this tier": a stray HAX_FOO= falls through to the file
     * tier rather than shadowing it (or reading as a blank port). */
    EXPECT(config_load("{\"providers.llamacpp.port\": \"9090\", \"bash\": {\"timeout\": \"5s\"},"
                       " \"show_reasoning\": true}") == 0);
    setenv("HAX_LLAMACPP_PORT", "", 1);
    setenv("HAX_BASH_TIMEOUT", "", 1);
    setenv("HAX_SHOW_REASONING", "", 1);
    EXPECT_STR_EQ(config_str_nonempty("providers.llamacpp.port"), "9090");
    EXPECT(config_duration_ms("bash.timeout") == 5 * 1000);
    EXPECT(config_bool("show_reasoning") == 1);
    /* With no file value either, the registry default applies. */
    EXPECT(config_load(NULL) == 0);
    EXPECT_STR_EQ(config_str_nonempty("providers.llamacpp.port"), "8080");
    EXPECT(config_duration_ms("bash.timeout") == 120 * 1000);
    EXPECT(config_bool("show_reasoning") == 0);
    /* config_str applies the same policy now: an empty tier is skipped for a
     * setting where "" has no meaning, so the port reads its default rather
     * than a blank string. */
    EXPECT_STR_EQ(config_str("providers.llamacpp.port"), "8080");
    /* A setting that documents a meaning for empty keeps it verbatim. */
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    const char *sp = config_str("system_prompt");
    EXPECT(sp != NULL && *sp == '\0');
    clear_env();
}

static void test_override_beats_env(void)
{
    clear_env();
    config_load("{\"model\": \"from-file\"}");
    setenv("HAX_MODEL", "from-env", 1);
    /* A runtime override is the highest tier — it beats even an env var,
     * so a setting launched via env can still be changed at runtime. */
    config_set_override("model", "from-override");
    EXPECT_STR_EQ(config_str("model"), "from-override");
    /* Clearing the override falls back to env. */
    config_set_override("model", NULL);
    EXPECT_STR_EQ(config_str("model"), "from-env");
    unsetenv("HAX_MODEL");
}

static void test_state_tier_ordering(void)
{
    clear_env();
    config_set_override("model", NULL);
    /* The state tier (state.json) sits between env and the committed
     * config file: it overrides the declared default, but yields to an
     * explicit env var for a one-off invocation, and to a runtime override. */
    EXPECT(config_load("{\"model\": \"from-file\"}") == 0);
    EXPECT(config_load_state("{\"model\": \"from-state\"}") == 0);
    EXPECT_STR_EQ(config_str("model"), "from-state"); /* beats the file */
    setenv("HAX_MODEL", "from-env", 1);
    EXPECT_STR_EQ(config_str("model"), "from-env"); /* env still wins */
    config_set_override("model", "from-override");
    EXPECT_STR_EQ(config_str("model"), "from-override"); /* override is top */
    config_set_override("model", NULL);
    unsetenv("HAX_MODEL");
    EXPECT_STR_EQ(config_str("model"), "from-state");
    /* Same nested/flat grammar as the file tier. */
    EXPECT(config_load_state("{\"effort\": \"high\"}") == 0);
    EXPECT_STR_EQ(config_str("effort"), "high");
    /* Clearing the state tier falls back to the file. */
    EXPECT(config_load_state(NULL) == 0);
    EXPECT_STR_EQ(config_str("model"), "from-file");
    config_load(NULL);
}

/* A selection saved under a former provider id keeps its model/effort binding and survives
 * re-persisting under the canonical id. */
static void test_provider_binding_canonical_ids(void)
{
    clear_env();

    EXPECT(config_load(NULL) == 0);
    EXPECT(config_load_state("{\"provider\": \"llama.cpp\", \"model\": \"old-model\"}") == 0);
    setenv("HAX_PROVIDER", "llamacpp", 1);
    EXPECT_STR_EQ(config_str("model"), "old-model");
    unsetenv("HAX_PROVIDER");

    char *dir = t_tempdir();
    if (dir) {
        setenv("XDG_STATE_HOME", dir, 1);
        /* Selecting the canonical id is not a provider change: the saved model is kept
         * instead of being reset to the new provider's default. */
        EXPECT(config_persist_selection("llamacpp", NULL, NULL) == 0);
        EXPECT_STR_EQ(config_str("provider"), "llamacpp");
        EXPECT_STR_EQ(config_str("model"), "old-model");
        unsetenv("XDG_STATE_HOME");
    }

    EXPECT(config_load_state(NULL) == 0);
}

static void test_provider_binding(void)
{
    clear_env();

    /* model/effort saved by the selectors are bound to the provider recorded
     * with them: they apply while it is the active provider ... */
    EXPECT(config_load("{\"model\": \"from-file\"}") == 0);
    EXPECT(config_load_state("{\"provider\": \"openai\", \"model\": \"gpt-x\","
                             " \"effort\": \"high\"}") == 0);
    EXPECT_STR_EQ(config_str("provider"), "openai");
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    EXPECT_STR_EQ(config_str("effort"), "high");

    /* ... and a one-off HAX_PROVIDER skips them: resolution falls through to
     * the (unbound) file tier / registry default instead. */
    setenv("HAX_PROVIDER", "mock", 1);
    EXPECT_STR_EQ(config_str("model"), "from-file");
    EXPECT(config_str("effort") == NULL);
    /* An explicit env model is a deliberate pairing and always applies. */
    setenv("HAX_MODEL", "env-model", 1);
    EXPECT_STR_EQ(config_str("model"), "env-model");
    unsetenv("HAX_MODEL");
    unsetenv("HAX_PROVIDER");
    /* Dropping the one-off brings the saved pair back untouched. */
    EXPECT_STR_EQ(config_str("model"), "gpt-x");

    /* The file tier is bound the same way when it pairs model with provider:
     * a hand-written codex default doesn't leak into HAX_PROVIDER=mock. */
    EXPECT(config_load("{\"provider\": \"codex\", \"model\": \"gpt-x\","
                       " \"effort\": \"high\"}") == 0);
    EXPECT(config_load_state(NULL) == 0);
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    setenv("HAX_PROVIDER", "mock", 1);
    EXPECT(config_str("model") == NULL);
    EXPECT(config_str("effort") == NULL);
    unsetenv("HAX_PROVIDER");

    /* A tier that records no provider is unbound: a bare "model" in
     * config.json is a global claim and applies under any provider. */
    EXPECT(config_load("{\"model\": \"global\"}") == 0);
    setenv("HAX_PROVIDER", "mock", 1);
    EXPECT_STR_EQ(config_str("model"), "global");
    unsetenv("HAX_PROVIDER");

    /* Cross-tier: the active provider resolving from the state tier skips a
     * file-tier model bound to a different file-tier provider. */
    EXPECT(config_load("{\"provider\": \"codex\", \"model\": \"codex-model\"}") == 0);
    EXPECT(config_load_state("{\"provider\": \"openai\"}") == 0);
    EXPECT(config_str("model") == NULL);

    /* No resolvable provider at all (empty HAX_PROVIDER → auto-select runs):
     * a bound value can't claim whatever gets inferred. */
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_load_state("{\"provider\": \"openai\", \"model\": \"gpt-x\"}") == 0);
    setenv("HAX_PROVIDER", "", 1);
    EXPECT(config_str("model") == NULL);
    unsetenv("HAX_PROVIDER");

    config_load(NULL);
    config_load_state(NULL);
}

static void test_persist_selection(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_STATE_HOME", dir, 1);
    setenv("XDG_CONFIG_HOME", dir, 1); /* keep config_init off the real file */

    /* A full pick lands as one write and reads back. */
    EXPECT(config_persist_selection("codex", "gpt-x", "high") == 0);
    EXPECT_STR_EQ(config_str("provider"), "codex");
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    EXPECT_STR_EQ(config_str("effort"), "high");

    /* Unpicked members (NULL) keep their stored value while the provider is
     * unchanged: an effort-only pick must not wipe the saved model. */
    EXPECT(config_persist_selection("codex", NULL, "low") == 0);
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    EXPECT_STR_EQ(config_str("effort"), "low");

    /* Re-pinning a different provider resets unpicked members to the
     * sentinel: the old provider's picks must not follow the new one. */
    EXPECT(config_persist_selection("mock", NULL, NULL) == 0);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT(config_str("model") == NULL);
    EXPECT(config_str("effort") == NULL);

    /* The reset is on disk, not just in memory. */
    config_load_state(NULL);
    config_init();
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT(config_str("model") == NULL);

    /* An explicit selection commit removes a persisted preset stance in the
     * same write — otherwise it would re-apply next launch and shadow the
     * very selection committed here. */
    EXPECT(config_persist_state("preset", "review") == 0);
    EXPECT_STR_EQ(config_str("preset"), "review");
    EXPECT(config_persist_selection("mock", "m2", NULL) == 0);
    EXPECT(config_str("preset") == NULL);

    /* A failed write leaves the in-memory tier unchanged (see
     * test_persist_failure_rolls_back for the same contract per-key). */
    setenv("XDG_STATE_HOME", "/dev/null/nope", 1);
    EXPECT(config_persist_selection("other", NULL, NULL) == -1);
    EXPECT_STR_EQ(config_str("provider"), "mock");

    /* A selection needs its provider anchor. */
    EXPECT(config_persist_selection(NULL, "gpt-x", NULL) == -1);
    EXPECT(config_persist_selection("", "gpt-x", NULL) == -1);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
    config_free();
}

static void test_default_sentinel(void)
{
    clear_env();
    config_set_override("model", NULL);
    config_load(NULL);
    config_load_state(NULL);

    /* CONFIG_VALUE_DEFAULT on a key without a registry default resolves to
     * NULL (consumer uses its own default) and shadows lower tiers instead
     * of falling through to them. */
    EXPECT(config_load("{\"model\": \"from-file\"}") == 0);
    EXPECT_STR_EQ(config_str("model"), "from-file");
    config_set_override("model", CONFIG_VALUE_DEFAULT);
    EXPECT(config_str("model") == NULL); /* sentinel, not "from-file" */
    /* The literal token round-trips through a tier the same way. */
    config_set_override("model", NULL);
    EXPECT(config_load_state("{\"model\": \"(default)\"}") == 0);
    EXPECT(config_str("model") == NULL);

    /* An override sentinel sits above env, so it shadows even an env var —
     * this is what stops a stale HAX_MODEL leaking into a switched provider
     * for the session. With the override cleared, env wins again. */
    setenv("HAX_MODEL", "from-env", 1);
    config_set_override("model", CONFIG_VALUE_DEFAULT);
    EXPECT(config_str("model") == NULL);
    config_set_override("model", NULL);
    EXPECT_STR_EQ(config_str("model"), "from-env");
    unsetenv("HAX_MODEL");

    /* On a key with a registry default the sentinel lands on that default
     * instead of NULL — same shadowing of lower tiers, one definition of
     * the default. */
    setenv("HAX_LLAMACPP_PORT", "9999", 1);
    EXPECT_STR_EQ(config_str("providers.llamacpp.port"), "9999");
    config_set_override("providers.llamacpp.port", CONFIG_VALUE_DEFAULT);
    EXPECT_STR_EQ(config_str("providers.llamacpp.port"), "8080");
    /* An empty override on this key is "unset" (a port has no meaning for
     * ""), so it falls through to the env value instead of reading blank —
     * distinct from the sentinel above, which lands on the default. */
    config_set_override("providers.llamacpp.port", "");
    EXPECT_STR_EQ(config_str("providers.llamacpp.port"), "9999");
    config_set_override("providers.llamacpp.port", NULL);
    unsetenv("HAX_LLAMACPP_PORT");

    /* A setting that documents a meaning for empty reads it back verbatim. */
    config_set_override("system_prompt", "");
    const char *sp = config_str("system_prompt");
    EXPECT(sp != NULL && *sp == '\0');
    config_set_override("system_prompt", NULL);

    config_load(NULL);
    config_load_state(NULL);
}

static void test_persist_state_roundtrip(void)
{
    clear_env();
    config_free();

    /* Separate temp trees for config and state so we can prove the
     * state-tier write lands in the state dir, not the config dir. */
    char *cfg_dir = t_tempdir();
    char *st_dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", cfg_dir, 1);
    setenv("XDG_STATE_HOME", st_dir, 1);

    EXPECT(config_persist_state("provider", "openrouter") == 0);
    EXPECT(config_persist_state("model", "some/model") == 0);

    /* It writes state.json (state dir), and leaves config.json absent. */
    char stpath[4096], cfgpath[4096];
    snprintf(stpath, sizeof stpath, "%s/hax/state.json", st_dir);
    snprintf(cfgpath, sizeof cfgpath, "%s/hax/config.json", cfg_dir);
    struct stat st;
    EXPECT(stat(stpath, &st) == 0 && (st.st_mode & 0777) == 0600);
    EXPECT(stat(cfgpath, &st) != 0);

    /* Reload from disk: the state tier reads back. */
    config_load(NULL);
    config_load_state(NULL);
    config_init();
    EXPECT_STR_EQ(config_str("provider"), "openrouter");
    EXPECT_STR_EQ(config_str("model"), "some/model");

    /* An env var still wins over a persisted selection (one-off override). */
    setenv("HAX_MODEL", "env-model", 1);
    EXPECT_STR_EQ(config_str("model"), "env-model");
    unsetenv("HAX_MODEL");

    /* Deleting a selection key removes it; resolution falls through. */
    EXPECT(config_persist_state("provider", NULL) == 0);
    EXPECT(config_str("provider") == NULL);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_persist_roundtrip(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", dir, 1);
    /* Isolate the state dir too: config_init() reads state.json (the
     * state tier) from XDG_STATE_HOME, and a developer's real
     * ~/.local/state/hax/state.json would otherwise shadow the config-file
     * values this test persists. The temp dir holds no state.json, so the
     * state tier stays empty. */
    setenv("XDG_STATE_HOME", dir, 1);

    /* Persist a flat and a nested key, then reload from disk. */
    EXPECT(config_persist("model", "saved-model") == 0);
    EXPECT(config_persist("providers.openai-compatible.base_url", "saved-url") == 0);
    config_load(NULL); /* drop the in-memory file tier */
    config_init();     /* read it back from disk */
    EXPECT_STR_EQ(config_str("model"), "saved-model");
    EXPECT_STR_EQ(config_str("providers.openai-compatible.base_url"), "saved-url");

    /* The file may hold API keys: it must be private (0600), and stay
     * private no matter what mode a stale temp file might have had. */
    char cfgpath[4096];
    snprintf(cfgpath, sizeof cfgpath, "%s/hax/config.json", dir);
    struct stat st;
    EXPECT(stat(cfgpath, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* A subsequent persist preserves the earlier keys. */
    EXPECT(config_persist("effort", "high") == 0);
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_str("model"), "saved-model");
    EXPECT_STR_EQ(config_str("effort"), "high");

    /* A hostile umask must not strip the 0600 contract: mkstemp's mode
     * is masked by the umask, the fchmod after it is not. (The config
     * dir already exists here — a 0777 umask would break mkdir itself,
     * which is not the scenario under test.) */
    mode_t prev_umask = umask(0777);
    EXPECT(config_persist("model", "saved-under-umask") == 0);
    umask(prev_umask);
    EXPECT(stat(cfgpath, &st) == 0 && (st.st_mode & 0777) == 0600);
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_str("model"), "saved-under-umask");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_persist_failure_rolls_back(void)
{
    clear_env();
    config_free();
    /* An unwritable XDG path makes the disk write fail; the in-memory
     * tier must keep the old value rather than claim one the disk never
     * saw. */
    setenv("XDG_CONFIG_HOME", "/dev/null/nope", 1);
    EXPECT(config_load("{\"model\": \"keep\"}") == 0);
    EXPECT(config_persist("model", "lost") == -1);
    EXPECT_STR_EQ(config_str("model"), "keep");
    unsetenv("XDG_CONFIG_HOME");
}

static void test_persist_flat_key(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1); /* isolate the state tier — see roundtrip test */

    /* A hand-written flat dotted key takes lookup precedence, so persist
     * must remove it or it would shadow the nested value it writes. On disk,
     * not merely loaded: a write merges into the file, so an in-memory-only
     * key would be dropped by the reload instead of by the removal under
     * test. */
    char cfgdir[2048], cfgpath[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(cfgpath, sizeof cfgpath, "%s/config.json", cfgdir);
    write_file(cfgpath, "{\"providers.openai-compatible.base_url\": \"old\"}");
    config_init();
    EXPECT(config_persist("providers.openai-compatible.base_url", "new") == 0);
    EXPECT_STR_EQ(config_str("providers.openai-compatible.base_url"), "new");
    config_load(NULL);
    config_init(); /* the rewritten file reads back the new value too */
    EXPECT_STR_EQ(config_str("providers.openai-compatible.base_url"), "new");

    /* Deleting a key written in flat form must actually delete it. */
    write_file(cfgpath, "{\"providers.openai-compatible.base_url\": \"old\"}");
    config_init();
    EXPECT(config_persist("providers.openai-compatible.base_url", NULL) == 0);
    EXPECT(config_str("providers.openai-compatible.base_url") == NULL);
    config_load(NULL);
    config_init();
    EXPECT(config_str("providers.openai-compatible.base_url") == NULL);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_registry_introspection(void)
{
    size_t count = 0;
    const struct config_setting *settings = config_settings(&count);
    EXPECT(settings != NULL);
    EXPECT(count > 0);

    for (size_t i = 0; i < count; i++) {
        const struct config_setting *setting = &settings[i];
        EXPECT(setting->key && *setting->key);
        EXPECT(setting->env_var && *setting->env_var);
        EXPECT(setting->description && *setting->description);
        if (setting->choices) {
            EXPECT(*setting->choices && setting->choices[strlen(setting->choices) - 1] != '|');
            EXPECT(!strstr(setting->choices, "||"));
        }
        if (setting->example) {
            EXPECT(setting->kind != CONFIG_KIND_STRING);
            EXPECT(setting->choices);
            EXPECT(config_value_valid(setting, setting->example));
        }
        if (setting->min || setting->max) {
            EXPECT(setting->kind != CONFIG_KIND_STRING);
            if (setting->min && setting->max)
                EXPECT(setting->min <= setting->max);
        }
        /* Editing secrets would expose them in the picker prompt. */
        if (setting->secret)
            EXPECT(!setting->editable);
        if (setting->keep_empty) {
            EXPECT(setting->kind == CONFIG_KIND_STRING);
            EXPECT(!(setting->choices && strcmp(setting->choices, CONFIG_CHOICES_BOOL) == 0));
        }
    }

    EXPECT(config_setting_find("system_prompt")->keep_empty);
    EXPECT(config_setting_find("system_prompt_append")->keep_empty);
    EXPECT(config_setting_find("effort")->keep_empty);
    EXPECT(!config_setting_find("theme")->keep_empty);
    EXPECT(!config_setting_find("sort_models")->keep_empty);
    const struct config_setting *show_reasoning = config_setting_find("show_reasoning");
    EXPECT(show_reasoning != NULL && show_reasoning->editable);
    EXPECT_STR_EQ(show_reasoning->choices, CONFIG_CHOICES_BOOL);
    EXPECT(config_setting_find("nonesuch") == NULL);
    EXPECT(config_setting_find("providers.openai-compatible.base_url") != NULL);
    EXPECT(!config_setting_find("providers.openai-compatible.base_url")->editable);
    const struct config_setting *api_key =
        config_setting_find("providers.openai-compatible.api_key");
    EXPECT(api_key != NULL && api_key->secret);
    EXPECT(!config_setting_find("markdown")->secret);
}

static void test_source_reports_winning_tier(void)
{
    clear_env();
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_load_state(NULL) == 0);

    /* Unset and registry-defaulted settings both report "default". */
    EXPECT_STR_EQ(config_source("show_reasoning"), "default");
    EXPECT_STR_EQ(config_source("providers.llamacpp.port"), "default");

    EXPECT(config_load("{\"show_reasoning\": \"1\"}") == 0);
    EXPECT_STR_EQ(config_source("show_reasoning"), "config");

    EXPECT(config_load_state("{\"show_reasoning\": \"0\"}") == 0);
    EXPECT_STR_EQ(config_source("show_reasoning"), "state");

    setenv("HAX_SHOW_REASONING", "1", 1);
    EXPECT_STR_EQ(config_source("show_reasoning"), "env");

    /* Run overrides win over the environment. */
    config_set_override("show_reasoning", "0");
    EXPECT_STR_EQ(config_source("show_reasoning"), "run");
    EXPECT(config_bool("show_reasoning") == 0);

    /* Clearing the override lets the tiers resurface in order. */
    config_set_override("show_reasoning", NULL);
    EXPECT_STR_EQ(config_source("show_reasoning"), "env");
    unsetenv("HAX_SHOW_REASONING");
    EXPECT_STR_EQ(config_source("show_reasoning"), "state");
    EXPECT(config_load_state(NULL) == 0);
    EXPECT_STR_EQ(config_source("show_reasoning"), "config");
    EXPECT(config_load(NULL) == 0);
    EXPECT_STR_EQ(config_source("show_reasoning"), "default");

    /* An empty tier is skipped for settings whose consumer skips it (numeric
     * and bool), so the reported source matches where the effective value
     * comes from — not the empty tier shadowing it. */
    EXPECT(config_load("{\"markdown\": \"0\", \"context_limit\": \"128k\"}") == 0);
    setenv("HAX_MARKDOWN", "", 1);
    setenv("HAX_CONTEXT_LIMIT", "", 1);
    EXPECT_STR_EQ(config_source("markdown"), "config"); /* empty env skipped */
    EXPECT(config_bool("markdown") == 0);               /* effective from file */
    EXPECT_STR_EQ(config_source("context_limit"), "config");
    EXPECT(config_tokens("context_limit") == 128000);
    /* A free-form setting keeps empty-as-meaningful: the empty env wins. */
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    EXPECT(config_load("{\"system_prompt\": \"from file\"}") == 0);
    EXPECT_STR_EQ(config_source("system_prompt"), "env");
    unsetenv("HAX_MARKDOWN");
    unsetenv("HAX_CONTEXT_LIMIT");
    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_string_and_integer_value_validation(void)
{
    char hint[64];

    const struct config_setting *system_prompt = config_setting_find("system_prompt");
    EXPECT(system_prompt && !system_prompt->choices && system_prompt->kind == CONFIG_KIND_STRING);
    EXPECT(config_value_valid(system_prompt, "anything"));
    EXPECT(!config_value_valid(system_prompt, NULL));
    EXPECT(!config_value_valid(NULL, "70"));
    config_value_hint(system_prompt, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "");

    const struct config_setting *threshold = config_setting_find("compact.threshold");
    EXPECT(threshold && !threshold->choices && threshold->kind == CONFIG_KIND_INT &&
           threshold->min == 1 && threshold->max == 100);
    EXPECT(config_value_valid(threshold, "70"));
    EXPECT(config_value_valid(threshold, "1"));
    EXPECT(config_value_valid(threshold, "100"));
    EXPECT(!config_value_valid(threshold, "0"));
    EXPECT(!config_value_valid(threshold, "200"));
    EXPECT(!config_value_valid(threshold, "banana"));
    EXPECT(!config_value_valid(threshold, "-5"));
    EXPECT(!config_value_valid(threshold, "12x"));
    EXPECT(!config_value_valid(threshold, ""));
    config_value_hint(threshold, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a whole number from 1 to 100");

    const struct config_setting *display_width = config_setting_find("display_width");
    EXPECT(display_width && display_width->kind == CONFIG_KIND_INT && display_width->min == 20);
    EXPECT_STR_EQ(display_width->choices, "auto|terminal");
    EXPECT_STR_EQ(display_width->example, "100");
    EXPECT(config_value_valid(display_width, "auto"));
    EXPECT(config_value_valid(display_width, "TERMINAL"));
    EXPECT(config_value_valid(display_width, "20"));
    EXPECT(config_value_valid(display_width, "500"));
    EXPECT(!config_value_valid(display_width, "19"));
    EXPECT(!config_value_valid(display_width, "wide"));
    config_value_hint(display_width, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "auto|terminal, or a whole number of at least 20; e.g. 100");
    char *canonical = config_value_canonical(display_width, "TERMINAL");
    EXPECT_STR_EQ(canonical, "terminal");
    free(canonical);
    EXPECT(config_value_canonical(display_width, "100") == NULL);

    /* Boolean aliases do not bypass bounds when attached to a numeric setting. */
    const struct config_setting mixed_bool = {
        .choices = CONFIG_CHOICES_BOOL,
        .kind = CONFIG_KIND_INT,
        .min = 20,
    };
    EXPECT(config_value_valid(&mixed_bool, "on"));
    EXPECT(config_value_valid(&mixed_bool, "20"));
    EXPECT(!config_value_valid(&mixed_bool, "1"));
    EXPECT(!config_value_valid(&mixed_bool, "true"));
    canonical = config_value_canonical(&mixed_bool, "ON");
    EXPECT_STR_EQ(canonical, "on");
    free(canonical);

    const struct config_setting *max_turns = config_setting_find("max_turns");
    EXPECT(max_turns && max_turns->kind == CONFIG_KIND_INT && max_turns->min == 0 &&
           max_turns->max == 0);
    EXPECT_STR_EQ(max_turns->default_value, "auto");
    EXPECT(config_value_valid(max_turns, "auto"));
    EXPECT(config_value_valid(max_turns, "0"));
    EXPECT(config_value_valid(max_turns, "25"));
    config_value_hint(max_turns, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "auto, or a whole number; e.g. 25");
}

static void test_bounded_and_scaled_value_validation(void)
{
    char hint[64];

    const struct config_setting *retention = config_setting_find("session_retention_days");
    EXPECT(retention && retention->kind == CONFIG_KIND_INT && retention->min == 0 &&
           retention->max == 36500);
    EXPECT_STR_EQ(retention->default_value, "30");
    EXPECT(config_value_valid(retention, "0"));
    EXPECT(config_value_valid(retention, "30"));
    EXPECT(!config_value_valid(retention, "36501"));

    const struct config_setting *max_retries = config_setting_find("http.max_retries");
    EXPECT(max_retries && max_retries->kind == CONFIG_KIND_INT && max_retries->min == 0 &&
           max_retries->max == 100);
    EXPECT(config_value_valid(max_retries, "100"));
    EXPECT(config_value_valid(max_retries, "0"));
    EXPECT(!config_value_valid(max_retries, "1000"));
    config_value_hint(max_retries, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a whole number up to 100");

    const struct config_setting *output_cap = config_setting_find("tool_output_cap");
    EXPECT(output_cap && output_cap->kind == CONFIG_KIND_SIZE);
    EXPECT(config_value_valid(output_cap, "64k"));
    EXPECT(config_value_valid(output_cap, "4096"));
    EXPECT(!config_value_valid(output_cap, "0"));
    EXPECT(!config_value_valid(output_cap, "lots"));
    config_value_hint(output_cap, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a byte size like 64k or 1M (k = 1024)");

    const struct config_setting *context_limit = config_setting_find("context_limit");
    EXPECT(context_limit && context_limit->kind == CONFIG_KIND_TOKENS);
    EXPECT(config_value_valid(context_limit, "872k"));
    EXPECT(!config_value_valid(context_limit, "lots"));
    config_value_hint(context_limit, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a token count like 200k or 1M (k = 1000)");

    const struct config_setting *timeout = config_setting_find("bash.timeout");
    EXPECT(timeout && timeout->kind == CONFIG_KIND_DURATION);
    EXPECT(config_value_valid(timeout, "2s"));
    EXPECT(config_value_valid(timeout, "500ms"));
    EXPECT(config_value_valid(timeout, "0"));
    EXPECT(!config_value_valid(timeout, "soon"));
    config_value_hint(timeout, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a duration like 2s or 500ms");

    /* A zero retry delay would create a tight retry loop. */
    const struct config_setting *retry_base = config_setting_find("http.retry_base");
    EXPECT(retry_base && retry_base->kind == CONFIG_KIND_DURATION && retry_base->min == 1);
    EXPECT(config_value_valid(retry_base, "1s"));
    EXPECT(!config_value_valid(retry_base, "0"));

    const struct config_setting *max_tokens =
        config_setting_find("providers.anthropic-compatible.max_tokens");
    EXPECT(max_tokens && !max_tokens->editable && max_tokens->kind == CONFIG_KIND_INT &&
           max_tokens->min == 1);
    EXPECT(config_value_valid(max_tokens, "32000"));
    EXPECT(!config_value_valid(max_tokens, "lots"));
    EXPECT(!config_value_valid(max_tokens, "0"));

    const struct config_setting *thinking_budget =
        config_setting_find("providers.anthropic-compatible.thinking_budget");
    EXPECT(thinking_budget && thinking_budget->kind == CONFIG_KIND_INT &&
           thinking_budget->min == 1 && thinking_budget->default_value == NULL);
    EXPECT(config_value_valid(thinking_budget, "1000"));
    EXPECT(!config_value_valid(thinking_budget, "0"));
}

static void test_choice_value_validation(void)
{
    const struct config_setting *system_prompt = config_setting_find("system_prompt");
    const struct config_setting *theme = config_setting_find("theme");
    char *canonical = config_value_canonical(theme, "LIGHT");
    EXPECT_STR_EQ(canonical, "light");
    free(canonical);
    EXPECT(config_value_canonical(theme, "nonesuch") == NULL);
    EXPECT(config_value_canonical(config_setting_find("markdown"), "ON") == NULL);
    EXPECT(config_value_canonical(system_prompt, "whatever") == NULL);

    const struct config_setting *markdown = config_setting_find("markdown");
    EXPECT(markdown && markdown->choices);
    EXPECT(config_value_valid(markdown, "on"));
    EXPECT(config_value_valid(markdown, "OFF"));
    EXPECT(config_value_valid(markdown, "1"));
    EXPECT(config_value_valid(markdown, "0"));
    EXPECT(config_value_valid(markdown, "true"));
    EXPECT(config_value_valid(markdown, "No"));
    EXPECT(!config_value_valid(markdown, "banana"));
    EXPECT(!config_value_valid(markdown, ""));

    EXPECT(theme && theme->choices);
    EXPECT(config_value_valid(theme, "dark"));
    EXPECT(config_value_valid(theme, "AUTO"));
    EXPECT(config_value_valid(theme, "off"));
    EXPECT(!config_value_valid(theme, "dar"));
    EXPECT(!config_value_valid(theme, "darker"));
    EXPECT(!config_value_valid(theme, ""));
}

static void test_sort_models_auto(void)
{
    clear_env();
    EXPECT(config_load(NULL) == 0);
    /* Unset resolves to the "auto" default, which config_bool_or treats as
     * unrecognized and so yields the caller's (provider) default — the /model
     * picker's actual behavior. So a provider defaulting on stays on. */
    const struct config_setting *sort_models = config_setting_find("sort_models");
    EXPECT(sort_models && sort_models->choices && strcmp(sort_models->choices, "on|off") != 0);
    EXPECT_STR_EQ(config_str("sort_models"), "auto");
    EXPECT(config_bool_or("sort_models", 1) == 1);
    EXPECT(config_bool_or("sort_models", 0) == 0);
    /* An explicit choice overrides the provider default either way. */
    EXPECT(config_load("{\"sort_models\": \"off\"}") == 0);
    EXPECT(config_bool_or("sort_models", 1) == 0);
    EXPECT(config_load("{\"sort_models\": \"on\"}") == 0);
    EXPECT(config_bool_or("sort_models", 0) == 1);

    /* Tri-state validation accepts "auto" plus the full bool grammar (so it
     * agrees with config_bool_or, which honors bool spellings and defers
     * everything else); a bogus value is invalid. The provider-defaulted cache
     * toggles share the exact shape. */
    EXPECT(config_value_valid(sort_models, "auto"));
    EXPECT(config_value_valid(sort_models, "AUTO"));
    EXPECT(config_value_valid(sort_models, "on"));
    EXPECT(config_value_valid(sort_models, "1"));
    EXPECT(config_value_valid(sort_models, "true"));
    EXPECT(config_value_valid(sort_models, "off"));
    EXPECT(!config_value_valid(sort_models, "banana"));
    EXPECT_STR_EQ(config_setting_find("providers.openai-compatible.send_cache_key")->choices,
                  CONFIG_CHOICES_TRISTATE);
    EXPECT_STR_EQ(config_setting_find("providers.openai-compatible.request_cost")->choices,
                  CONFIG_CHOICES_TRISTATE);
    EXPECT_STR_EQ(config_setting_find("providers.anthropic-compatible.cache")->choices,
                  CONFIG_CHOICES_TRISTATE);
}

static void test_empty_policy(void)
{
    clear_env();
    /* Enums and numerics treat an empty tier as unset, so a stray empty env
     * can't shadow a configured value or misreport its source. config_str,
     * config_source, and the consumers (theme, sort_models, notify) all agree
     * through the registry — no per-call-site skip-empty choice. */
    EXPECT(config_load("{\"theme\": \"light\", \"sort_models\": \"on\","
                       " \"notify\": \"bel\"}") == 0);
    setenv("HAX_THEME", "", 1);
    setenv("HAX_SORT_MODELS", "", 1);
    setenv("HAX_NOTIFY", "", 1);
    EXPECT_STR_EQ(config_str("theme"), "light");
    EXPECT_STR_EQ(config_source("theme"), "config");
    EXPECT_STR_EQ(config_str("sort_models"), "on");
    EXPECT_STR_EQ(config_source("sort_models"), "config");
    EXPECT(config_bool_or("sort_models", 0) == 1);
    EXPECT_STR_EQ(config_str("notify"), "bel");
    EXPECT_STR_EQ(config_source("notify"), "config");
    clear_env();

    /* Settings that document a meaning for empty keep it: the empty env wins
     * and is reported there, matching what the consumer reads. */
    EXPECT(config_load("{\"system_prompt\": \"from file\", \"effort\": \"high\","
                       " \"transcript\": \"/tmp/transcript\", \"trace\": \"/tmp/trace\"}") == 0);
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    setenv("HAX_EFFORT", "", 1);
    setenv("HAX_TRANSCRIPT", "", 1);
    setenv("HAX_TRACE", "", 1);
    const char *sp = config_str("system_prompt");
    EXPECT(sp && !*sp);
    EXPECT_STR_EQ(config_source("system_prompt"), "env");
    const char *ef = config_str("effort");
    EXPECT(ef && !*ef);
    const char *transcript = config_str("transcript");
    EXPECT(transcript && !*transcript);
    EXPECT_STR_EQ(config_source("transcript"), "env");
    const char *trace = config_str("trace");
    EXPECT(trace && !*trace);
    EXPECT_STR_EQ(config_source("trace"), "env");
    clear_env();
}

/* ---------- presets ---------- */

static void test_preset_apply(void)
{
    clear_env();
    EXPECT(config_load("{\"model\": \"base\", \"presets\": {"
                       "\"review\": {"
                       "\"description\": \"code review stance\","
                       "\"provider\": \"mock\","
                       "\"model\": \"rev-model\","
                       "\"effort\": \"high\","
                       "\"tint\": \"rose\","
                       "\"system_prompt\": \"you review code\"},"
                       "\"min\": {\"provider\": \"mock\"}}}") == 0);

    char *err = NULL;
    EXPECT(config_preset_apply("review", CONFIG_TIER_RUN, &err) == 0);
    EXPECT(err == NULL);
    /* Members land in the override tier — above env and the file tier. */
    setenv("HAX_MODEL", "env-model", 1);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT_STR_EQ(config_str("model"), "rev-model");
    EXPECT_STR_EQ(config_str("effort"), "high");
    EXPECT_STR_EQ(config_str("system_prompt"), "you review code");
    /* The persona's identity hue is read back off the stance, not written as
     * an override — the key has a second writer (/config tint) that stance
     * bookkeeping must not clobber. */
    EXPECT_STR_EQ(config_preset_tint("review"), "rose");
    EXPECT(strcmp(config_source("tint"), "run") != 0);
    /* The applied name is recorded as the active stance (banner, /session). */
    EXPECT_STR_EQ(config_str("preset"), "review");
    /* "description" is reserved metadata, not an override. */
    EXPECT(config_str("description") == NULL);
    EXPECT_STR_EQ(config_preset_description("review"), "code review stance");
    EXPECT_STR_EQ(config_preset_provider("review"), "mock");
    EXPECT_STR_EQ(config_preset_model("review"), "rev-model");
    EXPECT_STR_EQ(config_preset_effort("review"), "high");
    EXPECT_STR_EQ(config_preset_provider("min"), "mock");
    EXPECT(config_preset_model("min") == NULL);
    EXPECT(config_preset_effort("min") == NULL);

    /* A preset is a whole selection, so presets replace rather than
     * compose: one that names only the provider resets model/effort to the
     * sentinel — the provider's default applies and the env var must NOT
     * resurface — and clears the system_prompt override, so normal
     * resolution returns and the env var DOES resurface. */
    setenv("HAX_SYSTEM_PROMPT", "custom prompt", 1);
    setenv("HAX_TINT", "violet", 1);
    EXPECT(config_preset_apply("min", CONFIG_TIER_RUN, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT(config_str("model") == NULL);
    EXPECT(config_str("effort") == NULL);
    EXPECT_STR_EQ(config_str("system_prompt"), "custom prompt");
    /* A stance that names no hue leaves the user's own in force — applying
     * only clears the run tier, so env and the config file still resolve. */
    EXPECT(config_preset_tint("min") == NULL);
    EXPECT_STR_EQ(config_str("tint"), "violet");
    EXPECT_STR_EQ(config_str("preset"), "min");
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");
    unsetenv("HAX_TINT");

    /* Applying a stance does clear an explicit runtime tint, so its own hue
     * takes over: presets replace what was picked before them. */
    config_set_override("tint", "sage");
    EXPECT_STR_EQ(config_source("tint"), "run");
    EXPECT(config_preset_apply("review", CONFIG_TIER_RUN, &err) == 0);
    EXPECT(strcmp(config_source("tint"), "run") != 0);

    /* Leaving the stance must NOT take a runtime tint down with it. Unlike
     * system_prompt, /config writes this key too, and an explicit choice has
     * to outlive the /model or /provider pick that ends the stance. */
    config_set_override("tint", "sage");
    config_preset_exit(CONFIG_TIER_RUN);
    EXPECT_STR_EQ(config_str("preset"), "");
    EXPECT_STR_EQ(config_str("tint"), "sage");
    config_set_override("tint", NULL);

    /* Clear the applied overrides so later tests see a clean tier. */
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    config_set_override("system_prompt", NULL);
}

/* The conversation tier: what a resumed session recorded. Sits below run
 * overrides and above env, and is provider-bound like state/file. */
static void test_conversation_tier(void)
{
    clear_env();
    config_clear_conversation();
    EXPECT(config_load("{\"provider\": \"openai\", \"model\": \"file-model\"}") == 0);
    EXPECT(config_load_state("{\"provider\": \"anthropic\", \"model\": \"state-model\"}") == 0);
    EXPECT_STR_EQ(config_str("model"), "state-model");

    /* A resumed conversation outranks both persisted tiers. */
    config_set_conversation("provider", "codex");
    config_set_conversation("model", "gpt-5.1-codex");
    config_set_conversation("effort", "high");
    EXPECT_STR_EQ(config_source("provider"), "conversation");
    EXPECT_STR_EQ(config_str("provider"), "codex");
    EXPECT_STR_EQ(config_str("model"), "gpt-5.1-codex");
    EXPECT_STR_EQ(config_str("effort"), "high");

    /* ... env included: resuming continues the conversation on the backend
     * it was using, and configuration doesn't quietly redirect it. (A
     * subagent inherits its parent's HAX_* selection, so this is what keeps
     * `hax --resume=<child>` on the child's own setup.) */
    setenv("HAX_PROVIDER", "mock", 1);
    setenv("HAX_MODEL", "env-model", 1);
    EXPECT_STR_EQ(config_str("provider"), "codex");
    EXPECT_STR_EQ(config_str("model"), "gpt-5.1-codex");
    unsetenv("HAX_PROVIDER");
    unsetenv("HAX_MODEL");

    /* The selection flags are the escape hatch: a run override wins, and
     * --provider unpins the conversation's model/effort (bound to the
     * provider they were picked for) instead of pairing them with another
     * backend. */
    config_set_override("provider", "mock");
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT(config_str("model") == NULL);
    EXPECT(config_str("effort") == NULL);
    config_set_override("provider", NULL);
    EXPECT_STR_EQ(config_str("model"), "gpt-5.1-codex");

    /* --model alone keeps the conversation's provider. */
    config_set_override("model", "flag-model");
    EXPECT_STR_EQ(config_str("provider"), "codex");
    EXPECT_STR_EQ(config_str("model"), "flag-model");
    config_set_override("model", NULL);

    /* A restored preset lands in the same tier — the stance a resumed
     * conversation was running under, system prompt included. */
    EXPECT(config_load("{\"presets\": {\"review\": {\"provider\": \"mock\","
                       "\"model\": \"rev-model\", \"system_prompt\": \"you review\"}}}") == 0);
    char *err = NULL;
    EXPECT(config_preset_apply("review", CONFIG_TIER_CONVERSATION, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("preset"), "review");
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT_STR_EQ(config_str("model"), "rev-model");
    EXPECT_STR_EQ(config_str("system_prompt"), "you review");
    /* An explicit --model overrides just the model: the restored stance's
     * system prompt (and its provider) stay in effect, exactly as
     * `--preset review --model x` composes. */
    config_set_override("model", "flag-model");
    EXPECT_STR_EQ(config_str("model"), "flag-model");
    EXPECT_STR_EQ(config_str("system_prompt"), "you review");
    EXPECT_STR_EQ(config_str("preset"), "review");
    config_set_override("model", NULL);

    /* Clearing the tier drops the whole restore at once. */
    config_clear_conversation();
    EXPECT(config_str("preset") == NULL);
    EXPECT(config_str("system_prompt") == NULL);
    EXPECT(config_load_state(NULL) == 0);
    config_set_override("model", NULL);
}

/* config_restore_conversation: the write side of --resume, as the recorded
 * metadata comes off disk. */
static void test_restore_conversation(void)
{
    clear_env();
    config_clear_conversation();
    EXPECT(config_load("{\"presets\": {\"review\": {\"provider\": \"mock\","
                       "\"model\": \"rev-model\", \"system_prompt\": \"you review\"}}}") == 0);
    /* A stance persisted by /preset must not claim a conversation that ran
     * without one. */
    EXPECT(config_load_state("{\"preset\": \"review\", \"provider\": \"anthropic\","
                             "\"model\": \"state-model\"}") == 0);

    char *err = NULL;
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "codex", "gpt-5.1-codex", NULL, NULL,
                                    &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("provider"), "codex");
    EXPECT_STR_EQ(config_str("model"), "gpt-5.1-codex");
    /* A recorded absence means "the provider's own default", not "whatever
     * state.json saved" — the sentinel resolves to no value at all. */
    EXPECT(config_str("effort") == NULL);
    EXPECT_STR_EQ(config_str("preset"), "");

    /* A recorded stance is restored whole, system prompt included. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "mock", "rev-model", NULL, "review",
                                    &err) == 0);
    EXPECT_STR_EQ(config_str("preset"), "review");
    EXPECT_STR_EQ(config_str("system_prompt"), "you review");

    /* A stance that no longer applies reports why, and leaves the rest of
     * the selection restored so an interactive caller can carry on. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "mock", "rev-model", "high", "gone",
                                    &err) == -1);
    EXPECT(err != NULL);
    free(err);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT_STR_EQ(config_str("model"), "rev-model");
    EXPECT_STR_EQ(config_str("effort"), "high");
    EXPECT_STR_EQ(config_str("preset"), "");

    /* An explicit selection for the run exits the recorded stance — the
     * caller passes preset=NULL — and what's left has to be a state the run
     * can record and resume back into: the flag's value on top of the
     * conversation's own, with no preset to overwrite it next time. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "mock", "rev-model", NULL, NULL,
                                    &err) == 0);
    config_set_override("model", "flag-model");
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT_STR_EQ(config_str("model"), "flag-model");
    EXPECT_STR_EQ(config_str("preset"), "");
    EXPECT(config_str("system_prompt") == NULL);
    /* Resuming what that run records reproduces it exactly. */
    config_set_override("model", NULL);
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "mock", "flag-model", NULL, NULL,
                                    &err) == 0);
    EXPECT_STR_EQ(config_str("model"), "flag-model");
    EXPECT_STR_EQ(config_str("preset"), "");

    /* The same call writes the run tier for a mid-session /resume, where the
     * restore is itself the newest explicit act. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_RUN, "mock", "rev-model", NULL, "review", &err) ==
           0);
    EXPECT_STR_EQ(config_source("provider"), "run");
    EXPECT_STR_EQ(config_str("model"), "rev-model");
    EXPECT_STR_EQ(config_str("system_prompt"), "you review");
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    config_set_override("preset", NULL);
    config_set_override("system_prompt", NULL);

    /* Exiting a stance for the run has to end the restored one too: clearing
     * the run override alone would just unhide the conversation tier, leaving
     * the resumed preset's system prompt in force under a banner reporting no
     * stance. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "mock", "rev-model", NULL, "review",
                                    &err) == 0);
    EXPECT_STR_EQ(config_str("system_prompt"), "you review");
    config_preset_exit(CONFIG_TIER_RUN);
    EXPECT_STR_EQ(config_str("preset"), "");
    EXPECT(config_str("system_prompt") == NULL);
    config_set_override("preset", NULL);
    config_set_override("system_prompt", NULL);

    /* Same for replacing it with another preset that names no system prompt
     * of its own — the outgoing one must not show through. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "mock", "rev-model", NULL, "review",
                                    &err) == 0);
    EXPECT(config_load("{\"presets\": {"
                       "\"review\": {\"provider\": \"mock\", \"system_prompt\": \"you review\"},"
                       "\"plain\": {\"provider\": \"mock\"}}}") == 0);
    EXPECT(config_preset_apply("plain", CONFIG_TIER_RUN, &err) == 0);
    EXPECT_STR_EQ(config_str("preset"), "plain");
    EXPECT(config_str("system_prompt") == NULL);
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    config_set_override("system_prompt", NULL);

    /* A rejected switch restores both caller-writable tiers. A run-tier
     * stance change reaches into the conversation tier (config_preset_exit),
     * so a snapshot covering only the overrides would roll back to a state
     * that never existed: the resumed conversation's persona deleted, with
     * nothing having replaced it. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "mock", "rev-model", NULL, "review",
                                    &err) == 0);
    struct config_snapshot *snap = config_snapshot_take();
    EXPECT(config_preset_apply("plain", CONFIG_TIER_RUN, &err) == 0); /* prospective... */
    EXPECT_STR_EQ(config_str("preset"), "plain");
    EXPECT(config_str("system_prompt") == NULL);
    config_snapshot_restore(snap); /* ...and its provider wouldn't construct */
    EXPECT_STR_EQ(config_str("preset"), "review");
    EXPECT_STR_EQ(config_str("system_prompt"), "you review");
    EXPECT_STR_EQ(config_str("provider"), "mock");

    /* A provider-less recording ("none", or none at all) has no backend to
     * go back to: only the stance is pinned, and the run's own configuration
     * decides the rest. */
    config_clear_conversation();
    EXPECT(config_restore_selection(CONFIG_TIER_CONVERSATION, "none", "ghost-model", NULL, NULL,
                                    &err) == 0);
    EXPECT_STR_EQ(config_str("provider"), "anthropic");
    EXPECT_STR_EQ(config_str("model"), "state-model");
    EXPECT_STR_EQ(config_str("preset"), "");

    config_clear_conversation();
    EXPECT(config_load_state(NULL) == 0);
}

static void test_preset_apply_errors(void)
{
    clear_env();
    EXPECT(config_load("{\"presets\": {"
                       "\"endpoint\": {\"provider\": \"mock\", "
                       "\"providers.openai-compatible.base_url\": \"u\"},"
                       "\"nonscalar\": {\"provider\": \"mock\", \"model\": {\"id\": \"x\"}},"
                       "\"badd\": {\"provider\": \"mock\", \"description\": {\"text\": \"x\"}},"
                       "\"badtint\": {\"provider\": \"mock\", \"tint\": \"chartreuse\"},"
                       "\"anon\": {\"model\": \"x\"}}}") == 0);

    /* Unknown preset name. */
    char *err = NULL;
    EXPECT(config_preset_apply("nope", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL);
    free(err);
    EXPECT(config_preset_description("nope") == NULL);

    /* Only selection keys are presettable; all-or-nothing, so the valid
     * "provider" member must not have been applied either. */
    err = NULL;
    EXPECT(config_preset_apply("endpoint", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL && strstr(err, "not presettable") != NULL);
    free(err);
    EXPECT(config_str("provider") == NULL);
    EXPECT(config_str("providers.openai-compatible.base_url") == NULL);

    /* Non-scalar member. */
    err = NULL;
    EXPECT(config_preset_apply("nonscalar", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL);
    free(err);

    /* "description" skips the allowed-keys check but not the scalar one —
     * a structured description would silently read back as none. */
    err = NULL;
    EXPECT(config_preset_apply("badd", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL && strstr(err, "description") != NULL);
    free(err);

    /* An unknown hue is an error, not a silent fall back to the default
     * palette: the display layer resolves it long after apply returns, so
     * only validation can keep application all-or-nothing here. */
    err = NULL;
    EXPECT(config_preset_apply("badtint", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL && strstr(err, "chartreuse") != NULL);
    free(err);
    EXPECT(config_str("provider") == NULL);

    /* A preset must anchor a provider. */
    err = NULL;
    EXPECT(config_preset_apply("anon", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL && strstr(err, "provider") != NULL);
    free(err);
    EXPECT(config_str("model") == NULL);
}

static void test_preset_prompt_append(void)
{
    clear_env();
    EXPECT(config_load("{\"system_prompt_append\": \"global extra\", \"presets\": {"
                       "\"terse\": {\"provider\": \"mock\", \"system_prompt_append\": "
                       "\"be terse\"},"
                       "\"plain\": {\"provider\": \"mock\"},"
                       "\"mute\": {\"provider\": \"mock\", \"system_prompt_append\": \"\"}}}") ==
           0);

    char *err = NULL;
    EXPECT(config_preset_apply("terse", CONFIG_TIER_RUN, &err) == 0);
    EXPECT_STR_EQ(config_str("system_prompt_append"), "be terse");

    /* A preset without the member exposes normal resolution again. */
    EXPECT(config_preset_apply("plain", CONFIG_TIER_RUN, &err) == 0);
    EXPECT_STR_EQ(config_str("system_prompt_append"), "global extra");

    /* An explicit empty member silences the config-file amendment. */
    EXPECT(config_preset_apply("mute", CONFIG_TIER_RUN, &err) == 0);
    const char *append = config_str("system_prompt_append");
    EXPECT(append != NULL && *append == '\0');

    /* Leaving the stance restores normal resolution. */
    config_preset_exit(CONFIG_TIER_RUN);
    EXPECT_STR_EQ(config_str("system_prompt_append"), "global extra");

    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    EXPECT(config_load(NULL) == 0);
}

static void test_preset_prompt_file_validation(void)
{
    clear_env();
    EXPECT(config_load("{\"presets\": {"
                       "\"broken\": {\"provider\": \"mock\","
                       "\"system_prompt\": \"@/nonexistent/prompt.md\"},"
                       "\"broken2\": {\"provider\": \"mock\","
                       "\"system_prompt_append\": \"@/nonexistent/other.md\"}}}") == 0);

    /* All-or-nothing: an unreadable @file fails apply before anything lands. */
    char *err = NULL;
    EXPECT(config_preset_apply("broken", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL && strstr(err, "prompt file") != NULL);
    free(err);
    EXPECT(config_str("provider") == NULL);

    /* Enumeration skips both, but warns only about the defect no failed apply already put in
     * front of the user. Order-sensitive: this must be the first enumeration since
     * config_free — later ones never warn at all. */
    unsigned long diag_before = hax_diag_sequence();
    char **names = NULL;
    EXPECT(config_preset_names(&names) == 0);
    free(names);
    EXPECT(hax_diag_sequence() == diag_before + 1);

    EXPECT(config_load(NULL) == 0);
}

static void test_prompt_expand(void)
{
    char *err = NULL;
    char *text = config_prompt_expand("plain text", &err);
    EXPECT_STR_EQ(text, "plain text");
    EXPECT(err == NULL);
    free(text);

    char *path = xasprintf("%s/prompt.md", t_tempdir());
    write_file(path, "from a file\n\n");
    char *value = xasprintf("@%s", path);
    text = config_prompt_expand(value, &err);
    EXPECT_STR_EQ(text, "from a file");
    EXPECT(err == NULL);
    free(text);
    free(value);
    free(path);

    text = config_prompt_expand("@/nonexistent/prompt.md", &err);
    EXPECT(text == NULL);
    EXPECT(err != NULL && strstr(err, "/nonexistent/prompt.md") != NULL);
    free(err);
}

static void test_preset_enumeration(void)
{
    clear_env();
    EXPECT(config_load("{\"presets\": {\"a\": {\"provider\": \"mock\"},"
                       " \"b\": {\"provider\": \"mock\"}}}") == 0);
    char **names = NULL;
    size_t n = config_preset_names(&names);
    EXPECT(n == 2);
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);

    /* Enumerated ⊆ appliable: everything listed must survive the same
     * validation apply runs. A name spelled only as fully-flat leaves
     * cannot be assembled into a preset object, and a structurally invalid
     * definition (missing provider, unknown member) would fail on
     * selection — neither may be advertised in the picker or the prompt
     * listing. The one-level-flat block form remains both listed and
     * appliable. */
    EXPECT(config_load("{\"presets.flatleaf.provider\": \"mock\","
                       "\"presets.block\": {\"provider\": \"mock\"},"
                       "\"presets\": {"
                       "\"anon\": {\"model\": \"x\", \"description\": \"no provider\"},"
                       "\"badtint\": {\"provider\": \"mock\", \"tint\": \"chartreuse\"},"
                       "\"typo\": {\"provider\": \"mock\", \"modle\": \"x\"}}}") == 0);
    n = config_preset_names(&names);
    EXPECT(n == 1);
    if (n == 1)
        EXPECT_STR_EQ(names[0], "block");
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    char *err = NULL;
    EXPECT(config_preset_apply("flatleaf", CONFIG_TIER_RUN, &err) == -1);
    EXPECT(err != NULL);
    free(err);
}

static void test_preset_dotted_name(void)
{
    clear_env();
    /* A user-chosen name containing dots is a literal member, not a nested
     * path — anything enumeration lists must also apply. */
    EXPECT(config_load("{\"presets\": {\"review.v2\": "
                       "{\"provider\": \"mock\", \"model\": \"m\"}}}") == 0);
    char **names = NULL;
    size_t n = config_object_keys("presets", &names);
    EXPECT(n == 1);
    if (n == 1)
        EXPECT_STR_EQ(names[0], "review.v2");
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);

    char *err = NULL;
    EXPECT(config_preset_apply("review.v2", CONFIG_TIER_RUN, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT_STR_EQ(config_str("model"), "m");
    EXPECT_STR_EQ(config_str("preset"), "review.v2");
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    config_set_override("system_prompt", NULL);

    /* The flat-authored top-level form still resolves via the fallback. */
    EXPECT(config_load("{\"presets.flat\": {\"provider\": \"mock\"}}") == 0);
    EXPECT(config_preset_apply("flat", CONFIG_TIER_RUN, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    config_set_override("system_prompt", NULL);
}

static void test_preset_name_valid(void)
{
    /* A dot is a name character (preset_node takes names literally); a path
     * separator or a space is not. */
    EXPECT(config_preset_name_valid("review"));
    EXPECT(config_preset_name_valid("review.v2"));
    EXPECT(config_preset_name_valid("fast-scout_2"));
    EXPECT(!config_preset_name_valid(""));
    EXPECT(!config_preset_name_valid(NULL));
    EXPECT(!config_preset_name_valid(".hidden"));   /* must start alphanumeric */
    EXPECT(!config_preset_name_valid("-flaglike")); /* would read as an option */
    EXPECT(!config_preset_name_valid("two words"));
    EXPECT(!config_preset_name_valid("slash/es"));
    char long_name[80];
    memset(long_name, 'a', sizeof long_name);
    long_name[sizeof long_name - 1] = '\0';
    EXPECT(!config_preset_name_valid(long_name));
}

static void test_preset_save(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1); /* isolate the state tier — see roundtrip test */

    /* Seed and load unrelated content so the save must preserve the process's config snapshot. The
     * new preset must apply both immediately and after a reload. */
    char precfg[4096];
    snprintf(precfg, sizeof precfg, "%s/hax", dir);
    EXPECT(mkdir(precfg, 0700) == 0);
    snprintf(precfg, sizeof precfg, "%s/hax/config.json", dir);
    write_file(precfg, "{\"model\": \"keep-me\"}");
    config_init();
    struct config_preset def = {.provider = "mock",
                                .model = "m",
                                .effort = "high",
                                .tint = "rose",
                                .description = "saved from the session",
                                .system_prompt_append = "be terse"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");
    EXPECT(config_preset_exists("scout"));

    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_str("model"), "keep-me");
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");
    EXPECT_STR_EQ(config_preset_model("scout"), "m");
    EXPECT_STR_EQ(config_preset_effort("scout"), "high");
    EXPECT_STR_EQ(config_preset_tint("scout"), "rose");
    EXPECT_STR_EQ(config_preset_description("scout"), "saved from the session");
    EXPECT(config_preset_apply("scout", CONFIG_TIER_RUN, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("system_prompt_append"), "be terse");
    config_preset_exit(CONFIG_TIER_RUN);
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);

    /* A second save keeps the first preset and replaces only its own name.
     * Omitted members are absent rather than empty, so the provider's own
     * defaults apply when it is used. */
    struct config_preset min = {.provider = "mock"};
    EXPECT(config_preset_save("bare", &min, &err) == 0);
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");
    EXPECT(config_preset_model("bare") == NULL);
    EXPECT(config_preset_effort("bare") == NULL);
    EXPECT(config_preset_tint("bare") == NULL);
    EXPECT(config_preset_description("bare") == NULL);

    /* A dotted name is a literal member: what was saved must read back and
     * apply under the same spelling, not become nesting. */
    EXPECT(config_preset_save("review.v2", &def, &err) == 0);
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_preset_provider("review.v2"), "mock");
    EXPECT(config_preset_apply("review.v2", CONFIG_TIER_RUN, &err) == 0);
    config_preset_exit(CONFIG_TIER_RUN);
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);

    /* The file must stay private: it sits beside API keys in the same file. */
    char cfgpath[4096];
    snprintf(cfgpath, sizeof cfgpath, "%s/hax/config.json", dir);
    struct stat sb;
    EXPECT(stat(cfgpath, &sb) == 0 && (sb.st_mode & 0777) == 0600);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_save_errors(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);

    /* Validation is the same one apply runs, so a save can never write a
     * definition /preset would then reject. Nothing is written on failure. */
    char *err = NULL;
    struct config_preset anon = {.model = "m"};
    EXPECT(config_preset_save("anon", &anon, &err) == -1);
    EXPECT(err != NULL);
    free(err);
    err = NULL;
    EXPECT(!config_preset_exists("anon"));

    struct config_preset badtint = {.provider = "mock", .tint = "chartreuse"};
    EXPECT(config_preset_save("hue", &badtint, &err) == -1);
    EXPECT(err != NULL);
    free(err);
    err = NULL;
    EXPECT(!config_preset_exists("hue"));

    struct config_preset ok = {.provider = "mock"};
    EXPECT(config_preset_save("two words", &ok, &err) == -1);
    EXPECT(err != NULL);
    free(err);
    err = NULL;

    /* A flat-authored "presets.<name>" key is only a lookup fallback, so the
     * nested member a save writes already wins — but the old block must not be
     * left in the file, where it would read as the live definition to whoever
     * edits it next. On disk, since that is what the save merges into. */
    char cfgdir[2048], cfgpath[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(cfgpath, sizeof cfgpath, "%s/config.json", cfgdir);
    write_file(cfgpath, "{\"presets.flat\": {\"provider\": \"mock\", \"model\": \"old\"}}");
    config_init();
    EXPECT_STR_EQ(config_preset_model("flat"), "old");
    struct config_preset fresh = {.provider = "mock", .model = "new"};
    EXPECT(config_preset_save("flat", &fresh, &err) == 0);
    EXPECT_STR_EQ(config_preset_model("flat"), "new");
    size_t len = 0;
    char *written = fs_read_file(cfgpath, &len);
    EXPECT(written != NULL && strstr(written, "presets.flat") == NULL);
    free(written);
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_preset_model("flat"), "new");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_save_overwrites_external_edit(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);

    /* The file is rewritten from the tier this process holds, so an edit made
     * while the session ran is overwritten — the same way it had no effect on
     * the running session. Deliberate: one snapshot governs both. */
    char cfgdir[2048], cfgpath[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(cfgpath, sizeof cfgpath, "%s/config.json", cfgdir);
    write_file(cfgpath, "{\"model\": \"at-startup\"}");
    config_init();
    write_file(cfgpath, "{\"model\": \"edited-since\"}");

    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(err == NULL);
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_str("model"), "at-startup");
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_write_refuses_unusable_file(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    char cfgdir[2048], cfgpath[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(cfgpath, sizeof cfgpath, "%s/config.json", cfgdir);
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);

    /* A file that couldn't be read at startup leaves the tier empty, so a write
     * built from it would replace hand-authored content this process never saw.
     * Both writers refuse, and the file stays exactly as it is. */
    write_file(cfgpath, "{ this is not json");
    config_init();
    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == -1);
    EXPECT(err != NULL);
    free(err);
    err = NULL;
    EXPECT(config_persist("model", "m") == -1);
    size_t len = 0;
    char *still = fs_read_file(cfgpath, &len);
    EXPECT(still != NULL && strcmp(still, "{ this is not json") == 0);
    free(still);

    /* The verdict belongs to the last load, not to the process: a tier loaded
     * from text (what most tests drive) supersedes it, and so does a
     * config_init over a fixed-up file. */
    EXPECT(config_load("{\"model\": \"from-text\"}") == 0);
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(err == NULL);

    write_file(cfgpath, "{\"model\": \"fixed\"}");
    config_init();
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("model"), "fixed");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_save_rejects_state_definition(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);

    /* A hand-placed nested state-tier preset outranks the config file, so
     * writing the same name there would resolve to the state one — reporting a
     * save that isn't in effect. Refuse and name the file instead. (The flat
     * spelling doesn't outrank it — see
     * test_preset_save_state_flat_does_not_shadow.) */
    EXPECT(config_load_state("{\"presets\": {\"scout\": {\"provider\": \"mock\"}}}") == 0);
    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == -1);
    EXPECT(err != NULL && strstr(err, "state.json") != NULL);
    free(err);
    err = NULL;

    /* An unrelated name is unaffected. */
    EXPECT(config_preset_save("other", &def, &err) == 0);
    EXPECT_STR_EQ(config_preset_provider("other"), "mock");

    /* Nor is a name the state tier mentions with a non-object: that doesn't
     * shadow the file (preset_node falls through to it), so the write does take
     * effect and must not be refused. */
    EXPECT(config_load_state("{\"presets\": {\"junky\": \"draft\"}}") == 0);
    EXPECT(config_preset_save("junky", &def, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_preset_provider("junky"), "mock");
    config_load_state(NULL);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_save_follows_symlink(void)
{
    clear_env();
    config_free();

    /* config.json is commonly a symlink into a dotfiles repo. Writing must
     * land on the file the link names, leaving the link a link — renaming
     * over it would detach the config from the repo that manages it. */
    char *dir = t_tempdir();
    char cfgdir[2048], link[4096], real[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(link, sizeof link, "%s/config.json", cfgdir);
    snprintf(real, sizeof real, "%s/dotfiles-config.json", dir);
    FILE *fp = fopen(real, "w");
    EXPECT(fp != NULL);
    fputs("{\"model\": \"from-dotfiles\"}\n", fp);
    fclose(fp);
    EXPECT(symlink(real, link) == 0);

    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);
    config_init();
    EXPECT_STR_EQ(config_str("model"), "from-dotfiles");

    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == 0);

    struct stat sb;
    EXPECT(lstat(link, &sb) == 0 && S_ISLNK(sb.st_mode));
    config_load(NULL);
    config_init(); /* reads through the link — i.e. the real file was written */
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");
    EXPECT_STR_EQ(config_str("model"), "from-dotfiles");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_save_follows_dangling_symlink(void)
{
    clear_env();
    config_free();

    /* The link points at a file that doesn't exist yet — a dotfiles repo whose
     * config.json hasn't been created. realpath(3) refuses such a chain, and
     * falling back to the link path would replace the link with a regular
     * file; the write must create the target instead. */
    char *dir = t_tempdir();
    char cfgdir[2048], link[4096], real[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(link, sizeof link, "%s/config.json", cfgdir);
    snprintf(real, sizeof real, "%s/dotfiles-config.json", dir);
    EXPECT(symlink(real, link) == 0);

    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);
    config_init();

    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(err == NULL);

    struct stat sb;
    EXPECT(lstat(link, &sb) == 0 && S_ISLNK(sb.st_mode)); /* still a link */
    EXPECT(stat(real, &sb) == 0 && S_ISREG(sb.st_mode));  /* target created */
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_persist_into_empty_file(void)
{
    clear_env();
    config_free();

    /* An existing but empty file is an empty tier at startup, so a write must
     * read it the same way rather than refusing everything. */
    char *dir = t_tempdir();
    char cfgdir[2048], stdir[2048], path[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(path, sizeof path, "%s/config.json", cfgdir);
    write_file(path, "\n  \n"); /* whitespace only is empty, not malformed */
    snprintf(stdir, sizeof stdir, "%s/state", dir);
    EXPECT(mkdir(stdir, 0700) == 0);
    snprintf(path, sizeof path, "%s/hax", stdir);
    EXPECT(mkdir(path, 0700) == 0);
    snprintf(path, sizeof path, "%s/hax/state.json", stdir);
    write_file(path, "");

    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", stdir, 1);
    config_init();

    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");
    EXPECT(config_persist_state("provider", "mock") == 0);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    /* Zero-byte reads the same way, in either file. */
    snprintf(path, sizeof path, "%s/config.json", cfgdir);
    write_file(path, "");
    config_init();
    EXPECT(config_preset_save("scout2", &def, &err) == 0);
    EXPECT(err == NULL);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_exists_counts_any_member(void)
{
    clear_env();
    /* A half-written definition — a string where an object belongs — is still
     * the user's content under that name. It isn't appliable (enumeration skips
     * it), but a save must see that the name is taken rather than replace it
     * without asking. */
    EXPECT(config_load("{\"presets\": {\"work\": \"draft\"}}") == 0);
    EXPECT(config_preset_exists("work"));
    EXPECT(config_preset_provider("work") == NULL); /* nothing appliable there */
    char **names = NULL;
    EXPECT(config_preset_names(&names) == 0);
    free(names);

    /* The flat spelling counts the same way, and an unrelated name doesn't. */
    EXPECT(config_load("{\"presets.work\": 7}") == 0);
    EXPECT(config_preset_exists("work"));
    EXPECT(!config_preset_exists("other"));
    EXPECT(!config_preset_exists(""));
    EXPECT(!config_preset_exists(NULL));

    /* A non-object in the state tier does not shadow a config-file definition
     * (preset_node falls through to the file), so it must not make a save
     * refuse — only an appliable object there does. */
    EXPECT(config_load("{\"presets\": {\"work\": {\"provider\": \"mock\"}}}") == 0);
    EXPECT(config_load_state("{\"presets\": {\"work\": \"junk\"}}") == 0);
    EXPECT_STR_EQ(config_preset_provider("work"), "mock");
    config_load_state(NULL);
}

static void test_write_fails_on_unresolvable_link(void)
{
    clear_env();
    config_free();

    /* fs_resolve_link_target reports a hard failure by returning NULL. Falling
     * back to the link path would rename over the very link the resolution
     * exists to preserve, so the write must fail instead.
     *
     * A chain longer than the resolver's 32-hop cap but shorter than the
     * kernel's own limit is the case that reaches this: reading the file at
     * startup succeeds (here it ends at a missing target, so the tier is just
     * empty — no unusable-file verdict to refuse on), while resolution gives
     * up. */
    char *dir = t_tempdir();
    char cfgdir[2048], link[4096], next[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(link, sizeof link, "%s/config.json", cfgdir);
    /* config.json -> hop1 -> ... -> hop33 (absent) */
    for (int i = 1; i <= 33; i++) {
        snprintf(next, sizeof next, "%s/hop%d", cfgdir, i);
        const char *from = (i == 1) ? link : NULL;
        char prev[4096];
        if (!from) {
            snprintf(prev, sizeof prev, "%s/hop%d", cfgdir, i - 1);
            from = prev;
        }
        EXPECT(symlink(next, from) == 0);
    }

    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);
    config_init();

    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == -1);
    EXPECT(err != NULL);
    free(err);
    EXPECT(config_persist("model", "m") == -1);
    struct stat sb;
    EXPECT(lstat(link, &sb) == 0 && S_ISLNK(sb.st_mode)); /* still a link */

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_write_creates_link_target_directory(void)
{
    clear_env();
    config_free();

    /* config.json -> a dotfiles path whose directory doesn't exist yet: the
     * temp file is staged beside the *resolved* target, so that directory is
     * the one that has to be created. */
    char *dir = t_tempdir();
    char cfgdir[2048], link[4096], real[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(link, sizeof link, "%s/config.json", cfgdir);
    snprintf(real, sizeof real, "%s/dotfiles/hax/config.json", dir);
    EXPECT(symlink(real, link) == 0);

    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);
    config_init();

    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(err == NULL);

    struct stat sb;
    EXPECT(lstat(link, &sb) == 0 && S_ISLNK(sb.st_mode));
    EXPECT(stat(real, &sb) == 0 && S_ISREG(sb.st_mode));
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_save_refuses_bad_presets_container(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    char cfgdir[2048], cfgpath[4096];
    snprintf(cfgdir, sizeof cfgdir, "%s/hax", dir);
    EXPECT(mkdir(cfgdir, 0700) == 0);
    snprintf(cfgpath, sizeof cfgpath, "%s/config.json", cfgdir);
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);

    /* The file parses, so it is usable — but "presets" holds something that
     * isn't a block of them. Writing would drop it, and there is no name to
     * describe in the usual prompt, so the save refuses and the file stays. */
    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    const char *const bad[] = {"{\"presets\": \"draft\"}", "{\"presets\": [\"work\"]}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        write_file(cfgpath, bad[i]);
        config_init();
        EXPECT(config_preset_save("scout", &def, &err) == -1);
        EXPECT(err != NULL);
        free(err);
        err = NULL;
        size_t len = 0;
        char *still = fs_read_file(cfgpath, &len);
        EXPECT(still != NULL && strcmp(still, bad[i]) == 0);
        free(still);
    }

    /* A missing container is created, and a real one is added to. */
    write_file(cfgpath, "{\"model\": \"m\"}");
    config_init();
    EXPECT(config_preset_save("scout", &def, &err) == 0);
    EXPECT(config_preset_save("other", &def, &err) == 0);
    EXPECT_STR_EQ(config_preset_provider("scout"), "mock");
    EXPECT_STR_EQ(config_preset_provider("other"), "mock");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_preset_save_state_flat_does_not_shadow(void)
{
    clear_env();
    config_free();

    char *dir = t_tempdir();
    setenv("XDG_CONFIG_HOME", dir, 1);
    setenv("XDG_STATE_HOME", dir, 1);

    /* A *flat* state definition is preset_node's last-resort fallback, which the
     * nested member a save writes already beats — so the write does take effect
     * and must not be refused with a claim that state outranks it. */
    EXPECT(config_load_state("{\"presets.work\": {\"provider\": \"anthropic\"}}") == 0);
    struct config_preset def = {.provider = "mock", .model = "m"};
    char *err = NULL;
    EXPECT(config_preset_save("work", &def, &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_preset_provider("work"), "mock");

    /* A nested one does outrank it, so that save is still refused. */
    EXPECT(config_load_state("{\"presets\": {\"nest\": {\"provider\": \"anthropic\"}}}") == 0);
    EXPECT(config_preset_save("nest", &def, &err) == -1);
    EXPECT(err != NULL && strstr(err, "state.json") != NULL);
    free(err);
    config_load_state(NULL);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_str_below_run(void)
{
    clear_env();
    config_free();

    /* What a key resolves to with this run's own picks out of the way — for a
     * caller showing the result of an act that clears them. */
    EXPECT(config_load("{\"tint\": \"sage\"}") == 0);
    EXPECT_STR_EQ(config_str("tint"), "sage");
    EXPECT_STR_EQ(config_str_below_run("tint"), "sage"); /* nothing to skip */

    config_set_override("tint", "violet");
    EXPECT_STR_EQ(config_str("tint"), "violet");
    EXPECT_STR_EQ(config_str_below_run("tint"), "sage");

    /* Every lower tier still applies in order, and the registry default is the
     * floor once none of them names one. */
    setenv("HAX_TINT", "rose", 1);
    EXPECT_STR_EQ(config_str_below_run("tint"), "rose");
    unsetenv("HAX_TINT");
    EXPECT(config_load(NULL) == 0);
    EXPECT_STR_EQ(config_str_below_run("tint"), "teal");
    config_set_override("tint", NULL);
}

/* ---------- parse_size / parse_token_count / parse_duration_ms ---------- */

static void test_parse_size_basic(void)
{
    EXPECT(parse_size("4096") == 4096);
    EXPECT(parse_size("256k") == 256L * 1024);
    EXPECT(parse_size("128K") == 128L * 1024);
    EXPECT(parse_size("1m") == 1024L * 1024);
    EXPECT(parse_size("1M") == 1024L * 1024);
}

static void test_parse_token_count_is_decimal(void)
{
    EXPECT(parse_token_count("4096") == 4096);
    EXPECT(parse_token_count("256k") == 256000);
    EXPECT(parse_token_count("872K") == 872000);
    EXPECT(parse_token_count("1m") == 1000000);
    EXPECT(parse_token_count("1M") == 1000000);
    EXPECT(parse_token_count("xyz") == 0);
}

static void test_parse_size_invalid_returns_zero(void)
{
    EXPECT(parse_size(NULL) == 0);
    EXPECT(parse_size("") == 0);
    EXPECT(parse_size("xyz") == 0);
    EXPECT(parse_size("0") == 0);   /* explicit zero is still rejected */
    EXPECT(parse_size("-5k") == 0); /* negative */
    EXPECT(parse_size("5k junk") == 0);
}

static void test_parse_size_rejects_overflow(void)
{
    /* Numerals strtol clamps to LONG_MAX must NOT slip past — caller
     * would otherwise allocate / accept absurd cap values. */
    EXPECT(parse_size("99999999999999999999") == 0);
    EXPECT(parse_size("99999999999999999999k") == 0);
    /* Multiply-overflow guard: a value that fits in long but overflows
     * after the suffix-mul must be rejected. LONG_MAX / 1024 + 1 with
     * a 'k' suffix overflows. On 64-bit long, that's 9007199254740993k. */
    char buf[64];
    snprintf(buf, sizeof(buf), "%ldk", LONG_MAX / 1024L + 1);
    EXPECT(parse_size(buf) == 0);
    snprintf(buf, sizeof(buf), "%ldm", LONG_MAX / (1024L * 1024L) + 1);
    EXPECT(parse_size(buf) == 0);
}

static void test_parse_duration_plain_seconds(void)
{
    /* No suffix: number is interpreted as seconds, returned as ms. */
    EXPECT(parse_duration_ms("0") == 0);
    EXPECT(parse_duration_ms("30") == 30000);
    EXPECT(parse_duration_ms("600") == 600000);
}

static void test_parse_duration_with_suffix(void)
{
    EXPECT(parse_duration_ms("30s") == 30000);
    EXPECT(parse_duration_ms("30S") == 30000);
    EXPECT(parse_duration_ms("5m") == 300000);
    EXPECT(parse_duration_ms("5M") == 300000);
    EXPECT(parse_duration_ms("2h") == 7200000);
    EXPECT(parse_duration_ms("2H") == 7200000);
    /* `ms` must beat bare `m` so "250ms" isn't parsed as 250min + 's'. */
    EXPECT(parse_duration_ms("250ms") == 250);
    EXPECT(parse_duration_ms("250MS") == 250);
}

static void test_parse_duration_whitespace(void)
{
    EXPECT(parse_duration_ms("5 m") == 300000);
    EXPECT(parse_duration_ms("2h ") == 7200000);
    EXPECT(parse_duration_ms("100 ms") == 100);
}

static void test_parse_duration_invalid(void)
{
    EXPECT(parse_duration_ms(NULL) == -1);
    EXPECT(parse_duration_ms("") == -1);
    EXPECT(parse_duration_ms("abc") == -1);
    EXPECT(parse_duration_ms("5d") == -1);    /* days not supported */
    EXPECT(parse_duration_ms("-5") == -1);    /* negative rejected */
    EXPECT(parse_duration_ms("5 m x") == -1); /* trailing garbage */
    EXPECT(parse_duration_ms("5mm") == -1);
    EXPECT(parse_duration_ms("5msx") == -1); /* trailing after ms */
    /* strtol clamps to LONG_MAX with ERANGE; the ms suffix has mul==1
     * and would otherwise bypass the overflow guard. */
    EXPECT(parse_duration_ms("99999999999999999999ms") == -1);
    EXPECT(parse_duration_ms("99999999999999999999") == -1);
}

int main(void)
{
    test_load_validation();
    test_registry_introspection();
    test_source_reports_winning_tier();
    test_str_below_run();
    test_string_and_integer_value_validation();
    test_bounded_and_scaled_value_validation();
    test_choice_value_validation();
    test_sort_models_auto();
    test_empty_policy();
    test_nested_and_flat();
    test_scalar_normalization();
    test_typed_getters();
    test_registry_default();
    test_default_on_unset_and_invalid();
    test_env_wins_over_file();
    test_empty_means_unset();
    test_override_beats_env();
    test_state_tier_ordering();
    test_provider_binding();
    test_provider_binding_canonical_ids();
    test_default_sentinel();
    test_persist_state_roundtrip();
    test_persist_selection();
    test_persist_roundtrip();
    test_persist_failure_rolls_back();
    test_persist_flat_key();
    test_preset_apply();
    test_conversation_tier();
    test_restore_conversation();
    test_preset_apply_errors();
    test_preset_prompt_append();
    test_preset_prompt_file_validation();
    test_prompt_expand();
    test_preset_enumeration();
    test_preset_dotted_name();
    test_preset_name_valid();
    test_preset_save();
    test_preset_save_errors();
    test_preset_save_follows_symlink();
    test_preset_save_follows_dangling_symlink();
    test_write_creates_link_target_directory();
    test_write_fails_on_unresolvable_link();
    test_preset_exists_counts_any_member();
    test_preset_save_overwrites_external_edit();
    test_write_refuses_unusable_file();
    test_preset_save_rejects_state_definition();
    test_preset_save_state_flat_does_not_shadow();
    test_preset_save_refuses_bad_presets_container();
    test_persist_into_empty_file();
    config_free();

    test_parse_size_basic();
    test_parse_token_count_is_decimal();
    test_parse_size_invalid_returns_zero();
    test_parse_size_rejects_overflow();
    test_parse_duration_plain_seconds();
    test_parse_duration_with_suffix();
    test_parse_duration_whitespace();
    test_parse_duration_invalid();

    T_REPORT();
}
