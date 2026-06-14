/* SPDX-License-Identifier: MIT */
#include "config.h"

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include <jansson.h>

#include "util.h"
#include "system/fs.h"

/* The setting registry: the one place that knows a setting exists, which
 * env var binds it, its fixed default (NULL when the default is dynamic,
 * computed, or per-provider and the consumer supplies it), and a one-line
 * description (for help listings / a /config view). Adding a tunable means
 * adding a row here and reading it by its canonical key. Canonical keys
 * use '.' for grouping (openai.*, http.*, <provider>.*), which the file
 * can express as nested objects. Rows are grouped by area, and
 * config_settings() exposes them in this order — a help listing inherits
 * the grouping. Hand-aligned (clang-format off): key/env/default columns
 * on one line, description on the continuation line. */
// clang-format off
static const struct config_setting REGISTRY[] = {
    /* selection */
    {"provider",                   "HAX_PROVIDER",                NULL,
     "Backend: codex, openai, openai-compatible, llama.cpp, ollama, openrouter, mock"},
    {"model",                      "HAX_MODEL",                   NULL,
     "Model id (provider-specific; some auto-fill or require it)"},
    {"reasoning_effort",           "HAX_REASONING_EFFORT",        NULL,
     "Reasoning effort (minimal/low/medium/high/xhigh); empty omits it"},
    {"system_prompt",              "HAX_SYSTEM_PROMPT",           NULL,
     "Override the built-in system prompt; empty sends no system message"},
    {"no_env",                     "HAX_NO_ENV",                  NULL,
     "Skip the environment block in the system prompt"},
    {"no_agents_md",               "HAX_NO_AGENTS_MD",            NULL,
     "Skip AGENTS.md project instructions in the system prompt"},

    /* display */
    {"markdown",                   "HAX_MARKDOWN",                "1",
     "Render Markdown in the terminal (TTY only; piped output is always raw)"},
    {"show_reasoning",             "HAX_SHOW_REASONING",          NULL,
     "Show reasoning/CoT deltas live (default off)"},
    {"context_limit",              "HAX_CONTEXT_LIMIT",           NULL,
     "Manual context-window size for the % display (e.g. 256k); overrides auto-detect"},
    {"display_width",              "HAX_DISPLAY_WIDTH",           NULL,
     "Force content width in columns (default: terminal width, clamped)"},
    {"notify",                     "HAX_NOTIFY",                  NULL,
     "Desktop-notification style (auto-detected)"},

    /* behavior */
    {"keep_awake",                 "HAX_KEEP_AWAKE",              "1",
     "Inhibit idle system sleep while a turn is running (display may still blank)"},

    /* recording */
    {"no_session",                 "HAX_NO_SESSION",              NULL,
     "Disable session recording when set truthy"},
    {"transcript",                 "HAX_TRANSCRIPT",              NULL,
     "Path to mirror the Ctrl-T transcript view"},
    {"trace",                      "HAX_TRACE",                   NULL,
     "Path to a wire-level HTTP/SSE trace dump"},

    /* tools */
    {"tool_output_cap",            "HAX_TOOL_OUTPUT_CAP",         "50k",
     "Max bytes captured from a tool's output"},
    {"bash.timeout",               "HAX_BASH_TIMEOUT",            "2m",
     "Default bash-tool command timeout; 0 disables"},
    {"bash.timeout_max",           "HAX_BASH_TIMEOUT_MAX",        "30m",
     "Ceiling on the model's per-call bash timeout; 0 disables"},
    {"bash.timeout_grace",         "HAX_BASH_TIMEOUT_GRACE",      "2s",
     "Grace window between SIGTERM and SIGKILL for bash commands; 0 skips"},

    /* http transport */
    {"http.max_retries",           "HAX_HTTP_MAX_RETRIES",        "4",
     "Additional retries for transient HTTP failures"},
    {"http.retry_base",            "HAX_HTTP_RETRY_BASE",         "1s",
     "Base backoff between HTTP retries"},
    {"http.idle_timeout",          "HAX_HTTP_IDLE_TIMEOUT",       "10m",
     "Silence on a streaming response before giving up; 0 disables"},

    /* openai family (shared by the preset-based providers) */
    {"openai.base_url",            "HAX_OPENAI_BASE_URL",         NULL,
     "Base URL for the OpenAI-compatible endpoint"},
    {"openai.api_key",             "HAX_OPENAI_API_KEY",          NULL,
     "Bearer token for OpenAI-family providers"},
    {"openai.reasoning_format",    "HAX_OPENAI_REASONING_FORMAT", NULL,
     "Reasoning request dialect: flat or nested"},
    {"openai.reasoning_roundtrip", "HAX_REASONING_ROUNDTRIP",     NULL,
     "Replay reasoning text to the model (off/on, or a field name)"},
    {"openai.send_cache_key",      "HAX_OPENAI_SEND_CACHE_KEY",   NULL,
     "Send a stable prompt_cache_key (prefix-cache hint)"},
    {"provider_name",              "HAX_PROVIDER_NAME",           NULL,
     "Display name for the provider in the banner"},

    /* per-provider */
    {"llamacpp.port",              "HAX_LLAMACPP_PORT",           "8080",
     "Port for the local llama-server (when openai.base_url is unset)"},
    {"ollama.port",                "HAX_OLLAMA_PORT",             "11434",
     "Port for the local ollama daemon (when openai.base_url is unset)"},
    {"openrouter.title",           "HAX_OPENROUTER_TITLE",        NULL,
     "X-Title header for OpenRouter attribution"},
    {"openrouter.referer",         "HAX_OPENROUTER_REFERER",      NULL,
     "HTTP-Referer header for OpenRouter"},
    {"mock.script",                "HAX_MOCK_SCRIPT",             NULL,
     "Path to a mock-provider script (mock provider only)"},
};
// clang-format on

static const struct config_setting *find_setting(const char *key)
{
    for (size_t i = 0; i < sizeof(REGISTRY) / sizeof(REGISTRY[0]); i++) {
        if (strcmp(REGISTRY[i].key, key) == 0)
            return &REGISTRY[i];
    }
    return NULL;
}

const struct config_setting *config_settings(size_t *n)
{
    *n = sizeof(REGISTRY) / sizeof(REGISTRY[0]);
    return REGISTRY;
}

/* File tier (parsed config.json, always NULL or a JSON object) and the
 * session-only override tier (a flat object keyed by canonical key). */
static json_t *g_config;
static json_t *g_overrides;

#define CONFIG_MAX_BYTES (1024 * 1024)

/* Rewrite non-string scalar leaves as strings, so a value written as 5000
 * or true reads the same as "5000"/"1" and config_str can always hand back
 * a borrowed string. Recurses into nested objects; leaves arrays/null as-is
 * (they read as "unset"). */
static void normalize(json_t *obj)
{
    const char *k;
    json_t *v;
    void *tmp;
    json_object_foreach_safe(obj, tmp, k, v)
    {
        if (json_is_object(v)) {
            normalize(v);
        } else if (json_is_integer(v)) {
            char buf[32];
            snprintf(buf, sizeof buf, "%lld", (long long)json_integer_value(v));
            json_object_set_new(obj, k, json_string(buf));
        } else if (json_is_real(v)) {
            char buf[32];
            snprintf(buf, sizeof buf, "%g", json_real_value(v));
            json_object_set_new(obj, k, json_string(buf));
        } else if (json_is_boolean(v)) {
            json_object_set_new(obj, k, json_string(json_is_true(v) ? "1" : "0"));
        }
    }
}

int config_load(const char *text)
{
    json_decref(g_config);
    g_config = NULL;
    if (!text || !*text)
        return 0;
    json_t *root = json_loads(text, 0, NULL);
    if (!root)
        return -1;
    if (!json_is_object(root)) {
        json_decref(root);
        return -1;
    }
    normalize(root);
    g_config = root;
    return 0;
}

void config_init(void)
{
    char *path = xdg_hax_config_path("config.json");
    if (!path)
        return;
    size_t len;
    int truncated;
    errno = 0;
    char *data = slurp_file_capped(path, CONFIG_MAX_BYTES, &len, &truncated);
    if (data) {
        if (truncated)
            hax_warn("ignoring config at %s: larger than the 1 MiB limit", path);
        else if (config_load(data) != 0)
            hax_warn("ignoring malformed config at %s (expected a JSON object)", path);
        free(data);
    } else if (errno != ENOENT) {
        /* Absent is fine (config is optional); present-but-unreadable
         * (permissions, a directory, ...) must not be silent — the user
         * wrote a config and it isn't being honored. */
        hax_warn("ignoring unreadable config at %s: %s", path, strerror(errno));
    }
    free(path);
}

void config_free(void)
{
    json_decref(g_config);
    g_config = NULL;
    json_decref(g_overrides);
    g_overrides = NULL;
}

/* Look up `key` in the file tier: the flat literal key first (the
 * dotted-key form), then a walk down nested objects on '.'. Returns the
 * leaf string, or NULL. */
static const char *file_get(const char *key)
{
    if (!g_config)
        return NULL;
    json_t *v = json_object_get(g_config, key);
    if (!v) {
        json_t *cur = g_config;
        const char *p = key;
        for (;;) {
            const char *dot = strchr(p, '.');
            if (!dot) {
                v = json_object_get(cur, p);
                break;
            }
            char seg[64];
            size_t n = (size_t)(dot - p);
            if (n >= sizeof seg)
                return NULL;
            memcpy(seg, p, n);
            seg[n] = '\0';
            cur = json_object_get(cur, seg);
            if (!json_is_object(cur))
                return NULL;
            p = dot + 1;
        }
    }
    return json_string_value(v);
}

/* Walk the tiers: override (session) → environment → file → registry
 * default. With skip_empty, an empty value counts as "unset at this
 * tier" and resolution falls through to the next one — for settings
 * whose grammar gives "" no meaning (ports, durations, bools), where a
 * stray HAX_FOO= must not shadow a configured value or produce
 * nonsense. Without it, values are returned verbatim including "". */
static const char *resolve(const char *key, int skip_empty)
{
    const char *o = json_string_value(json_object_get(g_overrides, key));
    if (o && (!skip_empty || *o))
        return o;
    const struct config_setting *s = find_setting(key);
    if (s) {
        const char *e = getenv(s->env);
        if (e && (!skip_empty || *e))
            return e;
    }
    const char *f = file_get(key);
    if (f && (!skip_empty || *f))
        return f;
    return s ? s->def : NULL;
}

const char *config_str(const char *key)
{
    return resolve(key, 0);
}

const char *config_str_nonempty(const char *key)
{
    return resolve(key, 1);
}

const char *config_default(const char *key)
{
    const struct config_setting *s = find_setting(key);
    return s ? s->def : NULL;
}

/* The typed getters resolve skip-empty (resolve() already lands on the
 * registry default when every tier is unset or empty) and additionally
 * fall back to the registry default when the resolved value fails to
 * parse — a typo'd duration must not silently read as "0" and e.g.
 * disable a timeout. */

int config_int(const char *key)
{
    int v;
    /* Negative values read as invalid: every int setting is a count or
     * width, so "-1" is a typo, not a meaning — it must not be silently
     * honored as something else (e.g. "no retries"). */
    if (parse_int(resolve(key, 1), &v) && v >= 0)
        return v;
    const struct config_setting *s = find_setting(key);
    return s && parse_int(s->def, &v) ? v : 0;
}

/* Shared boolean grammar. Returns 1/0 for a recognized spelling, -1
 * otherwise. Both directions are explicit — most bool settings are no_*
 * switches, so a typo'd value reading as "true" would silently disable
 * sessions or strip the system prompt. */
static int parse_bool(const char *s)
{
    if (!s || !*s)
        return -1;
    if (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
        strcasecmp(s, "on") == 0)
        return 1;
    if (strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0 ||
        strcasecmp(s, "off") == 0)
        return 0;
    return -1;
}

int config_bool(const char *key)
{
    int v = parse_bool(resolve(key, 1));
    if (v < 0) {
        const struct config_setting *s = find_setting(key);
        v = s ? parse_bool(s->def) : -1;
    }
    return v > 0;
}

int config_bool_or(const char *key, int def)
{
    int v = parse_bool(resolve(key, 1));
    return v < 0 ? !!def : v;
}

long config_size(const char *key)
{
    long v = parse_size(resolve(key, 1)); /* NULL-safe; 0 = unset or invalid */
    if (v == 0) {
        const struct config_setting *s = find_setting(key);
        if (s)
            v = parse_size(s->def);
    }
    return v;
}

long config_duration_ms(const char *key)
{
    long v = parse_duration_ms(resolve(key, 1)); /* NULL-safe; -1 = unset or invalid */
    if (v < 0) {
        const struct config_setting *s = find_setting(key);
        v = s ? parse_duration_ms(s->def) : -1;
    }
    return v < 0 ? 0 : v;
}

void config_set_override(const char *key, const char *val)
{
    if (!g_overrides)
        g_overrides = json_object();
    if (val)
        json_object_set_new(g_overrides, key, json_string(val));
    else
        json_object_del(g_overrides, key);
}

/* Set (or, for val==NULL, delete) the nested leaf at canonical `key`,
 * creating intermediate objects as needed. */
static void set_nested(json_t *root, const char *key, const char *val)
{
    json_t *cur = root;
    const char *p = key;
    for (;;) {
        const char *dot = strchr(p, '.');
        if (!dot) {
            if (val)
                json_object_set_new(cur, p, json_string(val));
            else
                json_object_del(cur, p);
            return;
        }
        char seg[64];
        size_t n = (size_t)(dot - p);
        if (n >= sizeof seg)
            return;
        memcpy(seg, p, n);
        seg[n] = '\0';
        json_t *next = json_object_get(cur, seg);
        if (!json_is_object(next)) {
            next = json_object();
            json_object_set_new(cur, seg, next);
        }
        cur = next;
        p = dot + 1;
    }
}

/* Dump `obj` to `path` via a sibling temp file + rename, mode 0600 (config
 * may hold API keys). mkstemp, not open(O_CREAT): a unique name created
 * 0600 regardless of what existed before — a stale predictable .tmp could
 * otherwise carry a permissive mode through rename() onto config.json.
 * Returns 0 on success, -1 on any I/O failure. */
static int write_json_atomic(const char *path, json_t *obj)
{
    char *dup = xstrdup(path);
    fs_mkdir_p(dirname(dup));
    free(dup);

    char *tmp = xasprintf("%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    /* mkstemp's 0600 is still masked by the process umask; fchmod is
     * not. Pin the contract exactly — neither looser than 0600 nor a
     * hostile-umask 000 that rename() would promote onto config.json. */
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    int rc = json_dumpf(obj, fp, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    if (fclose(fp) != 0)
        rc = -1;
    if (rc == 0 && rename(tmp, path) != 0)
        rc = -1;
    if (rc != 0)
        unlink(tmp);
    free(tmp);
    return rc;
}

int config_persist(const char *key, const char *val)
{
    /* Mutate a copy and swap it in only after the write succeeds, so a
     * failed write can't leave the in-memory tier claiming a value the
     * disk never saw. */
    json_t *next = g_config ? json_deep_copy(g_config) : json_object();
    if (!next)
        return -1;
    /* A hand-written flat dotted key ({"openai.base_url": ...}) shadows
     * the nested path in file_get, so drop it — otherwise it would mask
     * the value persisted below (or survive a deletion). Rewriting the
     * file also normalizes it to nested form as a side effect. */
    json_object_del(next, key);
    set_nested(next, key, val);

    char *path = xdg_hax_config_path("config.json");
    if (!path) {
        json_decref(next);
        return -1;
    }
    int rc = write_json_atomic(path, next);
    free(path);
    if (rc != 0) {
        json_decref(next);
        return -1;
    }
    json_decref(g_config);
    g_config = next;
    return 0;
}
