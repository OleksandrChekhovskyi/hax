/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "harness.h"

/* config_init() reads a file from the XDG path; the load + resolve layer
 * underneath is what carries the logic, so most tests drive config_load()
 * directly. Every registered env var is cleared first so a developer's or
 * CI's environment can't perturb the assertions — the registry itself is
 * the list, so new settings are covered automatically. */
static void clear_env(void)
{
    size_t n = 0;
    const struct config_setting *s = config_settings(&n);
    for (size_t i = 0; i < n; i++)
        unsetenv(s[i].env);
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
    EXPECT(config_load("{\"openai\": {\"base_url\": \"nested\"}}") == 0);
    EXPECT_STR_EQ(config_str("openai.base_url"), "nested");
    /* A flat dotted key is accepted too. */
    EXPECT(config_load("{\"openai.base_url\": \"flat\"}") == 0);
    EXPECT_STR_EQ(config_str("openai.base_url"), "flat");
}

static void test_scalar_normalization(void)
{
    clear_env();
    /* Numbers and bools read as strings, so the typed getters work whether
     * the file wrote 64000 or "64000", true or "1". */
    EXPECT(config_load("{\"context_limit\": 64000, \"show_reasoning\": true}") == 0);
    EXPECT(config_size("context_limit") == 64000);
    EXPECT(config_bool("show_reasoning") == 1);
    EXPECT(config_load("{\"show_reasoning\": false}") == 0);
    EXPECT(config_bool("show_reasoning") == 0);
}

static void test_typed_getters(void)
{
    clear_env();
    EXPECT(config_load("{\"context_limit\": \"64k\", \"display_width\": \"100\","
                       " \"show_reasoning\": \"0\"}") == 0);
    EXPECT(config_size("context_limit") == 64 * 1024);
    EXPECT(config_int("display_width") == 100);
    EXPECT(config_bool("show_reasoning") == 0); /* explicit "0" is false */
    /* Unset typed reads are type-zero. */
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_int("display_width") == 0);
    EXPECT(config_size("context_limit") == 0);
    EXPECT(config_bool("show_reasoning") == 0);
}

static void test_registry_default(void)
{
    clear_env();
    config_load(NULL);
    /* llamacpp.port has a fixed default in the registry. */
    EXPECT_STR_EQ(config_str("llamacpp.port"), "8080");
    /* File overrides the default. */
    EXPECT(config_load("{\"llamacpp\": {\"port\": \"9090\"}}") == 0);
    EXPECT_STR_EQ(config_str("llamacpp.port"), "9090");
    /* Env overrides both. */
    setenv("HAX_LLAMACPP_PORT", "7070", 1);
    EXPECT_STR_EQ(config_str("llamacpp.port"), "7070");
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
    EXPECT(config_load("{\"openai\": {\"send_cache_key\": \"maybe\"}}") == 0);
    EXPECT(config_bool_or("openai.send_cache_key", 1) == 1);
    EXPECT(config_bool_or("openai.send_cache_key", 0) == 0);
    EXPECT(config_load("{\"openai\": {\"send_cache_key\": \"off\"}}") == 0);
    EXPECT(config_bool_or("openai.send_cache_key", 1) == 0);
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_bool_or("openai.send_cache_key", 1) == 1); /* unset → def */
    /* No registry default → type-zero, as before. */
    EXPECT(config_load("{\"context_limit\": \"nope\"}") == 0);
    EXPECT(config_size("context_limit") == 0);
    /* config_default exposes the registry default tier directly. */
    EXPECT_STR_EQ(config_default("llamacpp.port"), "8080");
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
    EXPECT(config_load("{\"llamacpp\": {\"port\": \"9090\"}, \"bash\": {\"timeout\": \"5s\"},"
                       " \"show_reasoning\": true}") == 0);
    setenv("HAX_LLAMACPP_PORT", "", 1);
    setenv("HAX_BASH_TIMEOUT", "", 1);
    setenv("HAX_SHOW_REASONING", "", 1);
    EXPECT_STR_EQ(config_str_nonempty("llamacpp.port"), "9090");
    EXPECT(config_duration_ms("bash.timeout") == 5 * 1000);
    EXPECT(config_bool("show_reasoning") == 1);
    /* With no file value either, the registry default applies. */
    EXPECT(config_load(NULL) == 0);
    EXPECT_STR_EQ(config_str_nonempty("llamacpp.port"), "8080");
    EXPECT(config_duration_ms("bash.timeout") == 120 * 1000);
    EXPECT(config_bool("show_reasoning") == 0);
    /* config_str applies the same policy now: an empty tier is skipped for a
     * setting where "" has no meaning, so the port reads its default rather
     * than a blank string. */
    EXPECT_STR_EQ(config_str("llamacpp.port"), "8080");
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
    EXPECT_STR_EQ(config_str("llamacpp.port"), "9999");
    config_set_override("llamacpp.port", CONFIG_VALUE_DEFAULT);
    EXPECT_STR_EQ(config_str("llamacpp.port"), "8080");
    /* An empty override on this key is "unset" (a port has no meaning for
     * ""), so it falls through to the env value instead of reading blank —
     * distinct from the sentinel above, which lands on the default. */
    config_set_override("llamacpp.port", "");
    EXPECT_STR_EQ(config_str("llamacpp.port"), "9999");
    config_set_override("llamacpp.port", NULL);
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
    EXPECT(config_persist("openai.base_url", "saved-url") == 0);
    config_load(NULL); /* drop the in-memory file tier */
    config_init();     /* read it back from disk */
    EXPECT_STR_EQ(config_str("model"), "saved-model");
    EXPECT_STR_EQ(config_str("openai.base_url"), "saved-url");

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
     * must remove it or it would shadow the nested value it writes. */
    EXPECT(config_load("{\"openai.base_url\": \"old\"}") == 0);
    EXPECT(config_persist("openai.base_url", "new") == 0);
    EXPECT_STR_EQ(config_str("openai.base_url"), "new");
    config_load(NULL);
    config_init(); /* the rewritten file reads back the new value too */
    EXPECT_STR_EQ(config_str("openai.base_url"), "new");

    /* Deleting a key written in flat form must actually delete it. */
    EXPECT(config_load("{\"openai.base_url\": \"old\"}") == 0);
    EXPECT(config_persist("openai.base_url", NULL) == 0);
    EXPECT(config_str("openai.base_url") == NULL);
    config_load(NULL);
    config_init();
    EXPECT(config_str("openai.base_url") == NULL);

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

static void test_registry_introspection(void)
{
    /* The registry is the source for help / a /config view: every entry
     * must carry a key, env binding, and description (default may be NULL). */
    size_t n = 0;
    const struct config_setting *s = config_settings(&n);
    EXPECT(s != NULL);
    EXPECT(n > 0);
    for (size_t i = 0; i < n; i++) {
        EXPECT(s[i].key && *s[i].key);
        EXPECT(s[i].env && *s[i].env);
        EXPECT(s[i].desc && *s[i].desc);
        /* Choice lists classify a setting's valid values for the picker (when
         * runtime) and for validation either way, so they're decoupled from
         * runtime: read-only enums (reasoning_format, thinking_mode) carry
         * choices purely to validate. Each listed value must be non-empty. */
        if (s[i].choices) {
            EXPECT(*s[i].choices && s[i].choices[strlen(s[i].choices) - 1] != '|');
            EXPECT(!strstr(s[i].choices, "||"));
        }
        /* Numeric kinds type the free-form path. */
        if (s[i].kind != CFG_STRING)
            EXPECT(!s[i].choices);
        /* Bounds are numeric and well-formed. They serve validation too, so a
         * setting may declare a range without a registry default when its
         * consumer supplies its own fallback (thinking_budget → max_tokens-1). */
        if (s[i].min || s[i].max) {
            EXPECT(s[i].kind != CFG_STRING);
            if (s[i].min && s[i].max)
                EXPECT(s[i].min <= s[i].max);
        }
        /* Runtime editing would expose a secret in the prompt. */
        if (s[i].secret)
            EXPECT(!s[i].runtime);
        /* keep_empty only makes sense for a free-form string — a bool or
         * numeric empty is always unset. */
        if (s[i].keep_empty) {
            EXPECT(s[i].kind == CFG_STRING);
            EXPECT(!(s[i].choices && strcmp(s[i].choices, CONFIG_CHOICES_BOOL) == 0));
        }
    }
    /* A documented-empty setting carries the flag; an enum that falls back on
     * empty does not. */
    EXPECT(config_setting_find("system_prompt")->keep_empty);
    EXPECT(config_setting_find("effort")->keep_empty);
    EXPECT(!config_setting_find("theme")->keep_empty);
    EXPECT(!config_setting_find("sort_models")->keep_empty);
    const struct config_setting *sr = config_setting_find("show_reasoning");
    EXPECT(sr != NULL && sr->runtime);
    EXPECT_STR_EQ(sr->choices, CONFIG_CHOICES_BOOL);
    EXPECT(config_setting_find("nonesuch") == NULL);
    EXPECT(config_setting_find("openai.base_url") != NULL);
    EXPECT(!config_setting_find("openai.base_url")->runtime);
    const struct config_setting *sk = config_setting_find("openai.api_key");
    EXPECT(sk != NULL && sk->secret);
    EXPECT(!config_setting_find("markdown")->secret);
}

static void test_source_reports_winning_tier(void)
{
    clear_env();
    EXPECT(config_load(NULL) == 0);
    EXPECT(config_load_state(NULL) == 0);

    /* Unset and registry-defaulted settings both report "default". */
    EXPECT_STR_EQ(config_source("show_reasoning"), "default");
    EXPECT_STR_EQ(config_source("llamacpp.port"), "default");

    EXPECT(config_load("{\"show_reasoning\": \"1\"}") == 0);
    EXPECT_STR_EQ(config_source("show_reasoning"), "config");

    EXPECT(config_load_state("{\"show_reasoning\": \"0\"}") == 0);
    EXPECT_STR_EQ(config_source("show_reasoning"), "state");

    setenv("HAX_SHOW_REASONING", "1", 1);
    EXPECT_STR_EQ(config_source("show_reasoning"), "env");

    /* Session overrides win over the environment. */
    config_set_override("show_reasoning", "0");
    EXPECT_STR_EQ(config_source("show_reasoning"), "session");
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
    EXPECT(config_size("context_limit") == 128 * 1024);
    /* A free-form setting keeps empty-as-meaningful: the empty env wins. */
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    EXPECT(config_load("{\"system_prompt\": \"from file\"}") == 0);
    EXPECT_STR_EQ(config_source("system_prompt"), "env");
    unsetenv("HAX_MARKDOWN");
    unsetenv("HAX_CONTEXT_LIMIT");
    unsetenv("HAX_SYSTEM_PROMPT");
}

static void test_value_valid(void)
{
    char hint[64];

    /* Free-form strings accept any non-NULL value. */
    const struct config_setting *ff = config_setting_find("system_prompt");
    EXPECT(ff && !ff->choices && ff->kind == CFG_STRING);
    EXPECT(config_value_valid(ff, "anything"));
    EXPECT(!config_value_valid(ff, NULL));
    EXPECT(!config_value_valid(NULL, "70"));
    config_value_hint(ff, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, ""); /* nothing to reject, so no hint */

    /* Integer settings reject malformed, negative, and out-of-bounds values;
     * compact.threshold carries a 1..100 range that the hint names. */
    const struct config_setting *i = config_setting_find("compact.threshold");
    EXPECT(i && !i->choices && i->kind == CFG_INT && i->min == 1 && i->max == 100);
    EXPECT(config_value_valid(i, "70"));
    EXPECT(config_value_valid(i, "1"));
    EXPECT(config_value_valid(i, "100"));
    EXPECT(!config_value_valid(i, "0"));   /* below min */
    EXPECT(!config_value_valid(i, "200")); /* above max */
    EXPECT(!config_value_valid(i, "banana"));
    EXPECT(!config_value_valid(i, "-5")); /* counts/widths are non-negative */
    EXPECT(!config_value_valid(i, "12x"));
    EXPECT(!config_value_valid(i, ""));
    config_value_hint(i, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a whole number from 1 to 100");

    /* An unbounded int keeps the plain hint. */
    const struct config_setting *mt = config_setting_find("max_turns");
    EXPECT(mt && mt->kind == CFG_INT && mt->min == 0 && mt->max == 0);
    config_value_hint(mt, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a whole number");

    /* A max-only int names its ceiling; the value is rejected above it. */
    const struct config_setting *mr = config_setting_find("http.max_retries");
    EXPECT(mr && mr->kind == CFG_INT && mr->min == 0 && mr->max == 100);
    EXPECT(config_value_valid(mr, "100"));
    EXPECT(config_value_valid(mr, "0")); /* disabling retries is allowed */
    EXPECT(!config_value_valid(mr, "1000"));
    config_value_hint(mr, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a whole number up to 100");

    /* Sizes accept suffixes but reject zero. */
    const struct config_setting *sz = config_setting_find("tool_output_cap");
    EXPECT(sz && sz->kind == CFG_SIZE);
    EXPECT(config_value_valid(sz, "64k"));
    EXPECT(config_value_valid(sz, "4096"));
    EXPECT(!config_value_valid(sz, "0"));
    EXPECT(!config_value_valid(sz, "lots"));
    config_value_hint(sz, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a size like 64k or 1M");

    /* Durations accept suffixes and zero, unless a min forbids it. */
    const struct config_setting *d = config_setting_find("bash.timeout");
    EXPECT(d && d->kind == CFG_DURATION);
    EXPECT(config_value_valid(d, "2s"));
    EXPECT(config_value_valid(d, "500ms"));
    EXPECT(config_value_valid(d, "0")); /* 0 disables — allowed here */
    EXPECT(!config_value_valid(d, "soon"));
    config_value_hint(d, hint, sizeof(hint));
    EXPECT_STR_EQ(hint, "a duration like 2s or 500ms");

    /* retry_base bounds out zero (0 backoff would hammer the server). */
    const struct config_setting *rb = config_setting_find("http.retry_base");
    EXPECT(rb && rb->kind == CFG_DURATION && rb->min == 1);
    EXPECT(config_value_valid(rb, "1s"));
    EXPECT(!config_value_valid(rb, "0"));

    const struct config_setting *nr = config_setting_find("anthropic.max_tokens");
    EXPECT(nr && !nr->runtime && nr->kind == CFG_INT && nr->min == 1);
    EXPECT(config_value_valid(nr, "32000"));
    EXPECT(!config_value_valid(nr, "lots"));
    EXPECT(!config_value_valid(nr, "0")); /* zero is ignored by the consumer */

    /* thinking_budget bounds zero out too, though its default is computed by
     * the consumer (max_tokens - 1) rather than declared in the registry. */
    const struct config_setting *tb = config_setting_find("anthropic.thinking_budget");
    EXPECT(tb && tb->kind == CFG_INT && tb->min == 1 && tb->def == NULL);
    EXPECT(config_value_valid(tb, "1000"));
    EXPECT(!config_value_valid(tb, "0"));

    /* Canonicalization: strict enums store the exact choice spelling, so a
     * case-sensitive consumer matches; bool/free-form need none. */
    const struct config_setting *th = config_setting_find("theme");
    char *canon = config_value_canonical(th, "LIGHT");
    EXPECT_STR_EQ(canon, "light");
    free(canon);
    EXPECT(config_value_canonical(th, "nonesuch") == NULL);
    EXPECT(config_value_canonical(config_setting_find("markdown"), "ON") == NULL);
    EXPECT(config_value_canonical(ff, "whatever") == NULL);

    /* Bool settings admit the full config_bool grammar, both cases. */
    const struct config_setting *b = config_setting_find("markdown");
    EXPECT(b && b->choices);
    EXPECT(config_value_valid(b, "on"));
    EXPECT(config_value_valid(b, "OFF"));
    EXPECT(config_value_valid(b, "1"));
    EXPECT(config_value_valid(b, "0"));
    EXPECT(config_value_valid(b, "true"));
    EXPECT(config_value_valid(b, "No"));
    EXPECT(!config_value_valid(b, "banana"));
    EXPECT(!config_value_valid(b, ""));

    /* Enum settings are strict membership, case-insensitive. */
    const struct config_setting *e = config_setting_find("theme");
    EXPECT(e && e->choices);
    EXPECT(config_value_valid(e, "dark"));
    EXPECT(config_value_valid(e, "AUTO"));
    EXPECT(config_value_valid(e, "off")); /* last list element matches */
    EXPECT(!config_value_valid(e, "dar"));
    EXPECT(!config_value_valid(e, "darker"));
    EXPECT(!config_value_valid(e, ""));
}

static void test_sort_models_auto(void)
{
    clear_env();
    EXPECT(config_load(NULL) == 0);
    /* Unset resolves to the "auto" default, which config_bool_or treats as
     * unrecognized and so yields the caller's (provider) default — the /model
     * picker's actual behavior. So a provider defaulting on stays on. */
    const struct config_setting *s = config_setting_find("sort_models");
    EXPECT(s && s->choices && strcmp(s->choices, "on|off") != 0);
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
    EXPECT(config_value_valid(s, "auto"));
    EXPECT(config_value_valid(s, "AUTO"));
    EXPECT(config_value_valid(s, "on"));
    EXPECT(config_value_valid(s, "1"));
    EXPECT(config_value_valid(s, "true"));
    EXPECT(config_value_valid(s, "off"));
    EXPECT(!config_value_valid(s, "banana"));
    EXPECT_STR_EQ(config_setting_find("openai.send_cache_key")->choices, CONFIG_CHOICES_TRISTATE);
    EXPECT_STR_EQ(config_setting_find("openai.request_cost")->choices, CONFIG_CHOICES_TRISTATE);
    EXPECT_STR_EQ(config_setting_find("anthropic.cache")->choices, CONFIG_CHOICES_TRISTATE);
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
                       " \"openrouter\": {\"referer\": \"https://x\"},"
                       " \"transcript\": \"/tmp/transcript\", \"trace\": \"/tmp/trace\"}") == 0);
    setenv("HAX_SYSTEM_PROMPT", "", 1);
    setenv("HAX_EFFORT", "", 1);
    setenv("HAX_OPENROUTER_REFERER", "", 1);
    setenv("HAX_TRANSCRIPT", "", 1);
    setenv("HAX_TRACE", "", 1);
    const char *sp = config_str("system_prompt");
    EXPECT(sp && !*sp);
    EXPECT_STR_EQ(config_source("system_prompt"), "env");
    const char *ef = config_str("effort");
    EXPECT(ef && !*ef);
    const char *rf = config_str("openrouter.referer");
    EXPECT(rf && !*rf);
    EXPECT_STR_EQ(config_source("openrouter.referer"), "env");
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
                       "\"system_prompt\": \"you review code\"},"
                       "\"min\": {\"provider\": \"mock\"}}}") == 0);

    char *err = NULL;
    EXPECT(config_preset_apply("review", &err) == 0);
    EXPECT(err == NULL);
    /* Members land in the override tier — above env and the file tier. */
    setenv("HAX_MODEL", "env-model", 1);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT_STR_EQ(config_str("model"), "rev-model");
    EXPECT_STR_EQ(config_str("effort"), "high");
    EXPECT_STR_EQ(config_str("system_prompt"), "you review code");
    /* The applied name is recorded as the active stance (banner, /session). */
    EXPECT_STR_EQ(config_str("preset"), "review");
    /* "description" is reserved metadata, not an override. */
    EXPECT(config_str("description") == NULL);
    EXPECT_STR_EQ(config_preset_description("review"), "code review stance");

    /* A preset is a whole selection, so presets replace rather than
     * compose: one that names only the provider resets model/effort to the
     * sentinel — the provider's default applies and the env var must NOT
     * resurface — and clears the system_prompt override, so normal
     * resolution returns and the env var DOES resurface. */
    setenv("HAX_SYSTEM_PROMPT", "custom prompt", 1);
    EXPECT(config_preset_apply("min", &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT(config_str("model") == NULL);
    EXPECT(config_str("effort") == NULL);
    EXPECT_STR_EQ(config_str("system_prompt"), "custom prompt");
    EXPECT_STR_EQ(config_str("preset"), "min");
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");

    /* Clear the applied overrides so later tests see a clean tier. */
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    config_set_override("system_prompt", NULL);
}

static void test_preset_apply_errors(void)
{
    clear_env();
    EXPECT(config_load("{\"presets\": {"
                       "\"endpoint\": {\"provider\": \"mock\", \"openai.base_url\": \"u\"},"
                       "\"nonscalar\": {\"provider\": \"mock\", \"model\": {\"id\": \"x\"}},"
                       "\"badd\": {\"provider\": \"mock\", \"description\": {\"text\": \"x\"}},"
                       "\"anon\": {\"model\": \"x\"}}}") == 0);

    /* Unknown preset name. */
    char *err = NULL;
    EXPECT(config_preset_apply("nope", &err) == -1);
    EXPECT(err != NULL);
    free(err);
    EXPECT(config_preset_description("nope") == NULL);

    /* Only selection keys are presettable; all-or-nothing, so the valid
     * "provider" member must not have been applied either. */
    err = NULL;
    EXPECT(config_preset_apply("endpoint", &err) == -1);
    EXPECT(err != NULL && strstr(err, "not presettable") != NULL);
    free(err);
    EXPECT(config_str("provider") == NULL);
    EXPECT(config_str("openai.base_url") == NULL);

    /* Non-scalar member. */
    err = NULL;
    EXPECT(config_preset_apply("nonscalar", &err) == -1);
    EXPECT(err != NULL);
    free(err);

    /* "description" skips the allowed-keys check but not the scalar one —
     * a structured description would silently read back as none. */
    err = NULL;
    EXPECT(config_preset_apply("badd", &err) == -1);
    EXPECT(err != NULL && strstr(err, "description") != NULL);
    free(err);

    /* A preset must anchor a provider. */
    err = NULL;
    EXPECT(config_preset_apply("anon", &err) == -1);
    EXPECT(err != NULL && strstr(err, "provider") != NULL);
    free(err);
    EXPECT(config_str("model") == NULL);
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
                       "\"typo\": {\"provider\": \"mock\", \"modle\": \"x\"}}}") == 0);
    n = config_preset_names(&names);
    EXPECT(n == 1);
    if (n == 1)
        EXPECT_STR_EQ(names[0], "block");
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    char *err = NULL;
    EXPECT(config_preset_apply("flatleaf", &err) == -1);
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
    EXPECT(config_preset_apply("review.v2", &err) == 0);
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
    EXPECT(config_preset_apply("flat", &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("effort", NULL);
    config_set_override("system_prompt", NULL);
}

int main(void)
{
    test_load_validation();
    test_registry_introspection();
    test_source_reports_winning_tier();
    test_value_valid();
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
    test_default_sentinel();
    test_persist_state_roundtrip();
    test_persist_selection();
    test_persist_roundtrip();
    test_persist_failure_rolls_back();
    test_persist_flat_key();
    test_preset_apply();
    test_preset_apply_errors();
    test_preset_enumeration();
    test_preset_dotted_name();
    config_free();
    T_REPORT();
}
