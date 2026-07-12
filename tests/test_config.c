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
    /* config_str itself stays verbatim — see test_env_wins_over_file. */
    const char *e = config_str("llamacpp.port");
    EXPECT(e != NULL && *e == '\0');
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
    EXPECT(config_load_state("{\"reasoning_effort\": \"high\"}") == 0);
    EXPECT_STR_EQ(config_str("reasoning_effort"), "high");
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
                             " \"reasoning_effort\": \"high\"}") == 0);
    EXPECT_STR_EQ(config_str("provider"), "openai");
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    EXPECT_STR_EQ(config_str("reasoning_effort"), "high");

    /* ... and a one-off HAX_PROVIDER skips them: resolution falls through to
     * the (unbound) file tier / registry default instead. */
    setenv("HAX_PROVIDER", "mock", 1);
    EXPECT_STR_EQ(config_str("model"), "from-file");
    EXPECT(config_str("reasoning_effort") == NULL);
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
                       " \"reasoning_effort\": \"high\"}") == 0);
    EXPECT(config_load_state(NULL) == 0);
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    setenv("HAX_PROVIDER", "mock", 1);
    EXPECT(config_str("model") == NULL);
    EXPECT(config_str("reasoning_effort") == NULL);
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

static void rm_rf_state(const char *dir); /* defined below; shared cleanup */

static void test_persist_selection(void)
{
    clear_env();
    config_free();

    char tmpl[] = "/tmp/haxseltest.XXXXXX";
    char *dir = mkdtemp(tmpl);
    EXPECT(dir != NULL);
    if (!dir)
        return;
    setenv("XDG_STATE_HOME", dir, 1);
    setenv("XDG_CONFIG_HOME", dir, 1); /* keep config_init off the real file */

    /* A full pick lands as one write and reads back. */
    EXPECT(config_persist_selection("codex", "gpt-x", "high") == 0);
    EXPECT_STR_EQ(config_str("provider"), "codex");
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    EXPECT_STR_EQ(config_str("reasoning_effort"), "high");

    /* Unpicked members (NULL) keep their stored value while the provider is
     * unchanged: an effort-only pick must not wipe the saved model. */
    EXPECT(config_persist_selection("codex", NULL, "low") == 0);
    EXPECT_STR_EQ(config_str("model"), "gpt-x");
    EXPECT_STR_EQ(config_str("reasoning_effort"), "low");

    /* Re-pinning a different provider resets unpicked members to the
     * sentinel: the old provider's picks must not follow the new one. */
    EXPECT(config_persist_selection("mock", NULL, NULL) == 0);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT(config_str("model") == NULL);
    EXPECT(config_str("reasoning_effort") == NULL);

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
    rm_rf_state(dir);
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
     * the default. An explicit empty value still reads verbatim (the
     * opt-out state for settings where "" is meaningful). */
    setenv("HAX_LLAMACPP_PORT", "9999", 1);
    EXPECT_STR_EQ(config_str("llamacpp.port"), "9999");
    config_set_override("llamacpp.port", CONFIG_VALUE_DEFAULT);
    EXPECT_STR_EQ(config_str("llamacpp.port"), "8080");
    config_set_override("llamacpp.port", "");
    EXPECT_STR_EQ(config_str("llamacpp.port"), "");
    config_set_override("llamacpp.port", NULL);
    unsetenv("HAX_LLAMACPP_PORT");

    config_load(NULL);
    config_load_state(NULL);
}

static void rm_rf_config(const char *dir); /* defined below; shared cleanup */

static void rm_rf_state(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/hax/state.json", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/hax", dir);
    rmdir(path);
    rmdir(dir);
}

static void test_persist_state_roundtrip(void)
{
    clear_env();
    config_free();

    /* Separate temp trees for config and state so we can prove the
     * state-tier write lands in the state dir, not the config dir. */
    char cfg_tmpl[] = "/tmp/haxcfgtest.XXXXXX";
    char st_tmpl[] = "/tmp/haxsttest.XXXXXX";
    char *cfg_dir = mkdtemp(cfg_tmpl);
    char *st_dir = mkdtemp(st_tmpl);
    EXPECT(cfg_dir != NULL && st_dir != NULL);
    if (!cfg_dir || !st_dir)
        return;
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
    rm_rf_config(cfg_dir);
    rm_rf_state(st_dir);
}

static void rm_rf_config(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/hax/config.json", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/hax", dir);
    rmdir(path);
    rmdir(dir);
}

static void test_persist_roundtrip(void)
{
    clear_env();
    config_free();

    char tmpl[] = "/tmp/haxcfgtest.XXXXXX";
    char *dir = mkdtemp(tmpl);
    EXPECT(dir != NULL);
    if (!dir)
        return;
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
    EXPECT(config_persist("reasoning_effort", "high") == 0);
    config_load(NULL);
    config_init();
    EXPECT_STR_EQ(config_str("model"), "saved-model");
    EXPECT_STR_EQ(config_str("reasoning_effort"), "high");

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
    rm_rf_config(dir);
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

    char tmpl[] = "/tmp/haxcfgtest.XXXXXX";
    char *dir = mkdtemp(tmpl);
    EXPECT(dir != NULL);
    if (!dir)
        return;
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
    rm_rf_config(dir);
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
    }
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
                       "\"reasoning_effort\": \"high\","
                       "\"system_prompt\": \"you review code\"},"
                       "\"min\": {\"provider\": \"mock\"}}}") == 0);

    char *err = NULL;
    EXPECT(config_preset_apply("review", &err) == 0);
    EXPECT(err == NULL);
    /* Members land in the override tier — above env and the file tier. */
    setenv("HAX_MODEL", "env-model", 1);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    EXPECT_STR_EQ(config_str("model"), "rev-model");
    EXPECT_STR_EQ(config_str("reasoning_effort"), "high");
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
    EXPECT(config_str("reasoning_effort") == NULL);
    EXPECT_STR_EQ(config_str("system_prompt"), "custom prompt");
    EXPECT_STR_EQ(config_str("preset"), "min");
    unsetenv("HAX_MODEL");
    unsetenv("HAX_SYSTEM_PROMPT");

    /* Clear the applied overrides so later tests see a clean tier. */
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("reasoning_effort", NULL);
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
    config_set_override("reasoning_effort", NULL);
    config_set_override("system_prompt", NULL);

    /* The flat-authored top-level form still resolves via the fallback. */
    EXPECT(config_load("{\"presets.flat\": {\"provider\": \"mock\"}}") == 0);
    EXPECT(config_preset_apply("flat", &err) == 0);
    EXPECT(err == NULL);
    EXPECT_STR_EQ(config_str("provider"), "mock");
    config_set_override("preset", NULL);
    config_set_override("provider", NULL);
    config_set_override("model", NULL);
    config_set_override("reasoning_effort", NULL);
    config_set_override("system_prompt", NULL);
}

int main(void)
{
    test_load_validation();
    test_registry_introspection();
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
