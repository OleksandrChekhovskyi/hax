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

/* Canonical keys, env bindings, defaults, and /config metadata. Row order is
 * user-visible. Runtime settings must be read live or refreshed after edits;
 * numeric bounds are enforced by the typed getters. */
// clang-format off
static const struct config_setting REGISTRY[] = {
    /* selection */
    {.key = "preset", .env = "HAX_PRESET", .keep_empty = 1,
     .desc = "Preset from presets.<name> to apply at startup; empty disables"},
    {.key = "provider", .env = "HAX_PROVIDER", .keep_empty = 1,
     .desc = "Backend: codex, openai, openai-compatible, anthropic, anthropic-compatible, "
             "llama.cpp, ollama, openrouter, mock"},
    {.key = "model", .env = "HAX_MODEL", .keep_empty = 1,
     .desc = "Model id (provider-specific; some auto-fill or require it)"},
    {.key = "effort", .env = "HAX_EFFORT", .keep_empty = 1,
     .desc = "Reasoning effort (minimal/low/medium/high/xhigh); empty omits it"},
    {.key = "system_prompt", .env = "HAX_SYSTEM_PROMPT", .keep_empty = 1,
     .desc = "Override the built-in system prompt; empty sends no system message"},
    {.key = "no_env", .env = "HAX_NO_ENV", .choices = CONFIG_CHOICES_BOOL,
     .desc = "Skip the Environment section in the system prompt"},
    {.key = "no_agents_md", .env = "HAX_NO_AGENTS_MD", .choices = CONFIG_CHOICES_BOOL,
     .desc = "Skip AGENTS.md project instructions in the system prompt"},
    {.key = "no_skills", .env = "HAX_NO_SKILLS", .choices = CONFIG_CHOICES_BOOL,
     .desc = "Skip the skills listing in the system prompt"},
    {.key = "no_subagents", .env = "HAX_NO_SUBAGENTS", .choices = CONFIG_CHOICES_BOOL,
     .desc = "Skip the subagents section in the system prompt"},

    /* display */
    {.key = "markdown", .env = "HAX_MARKDOWN", .def = "1",
     .desc = "Render Markdown in the terminal (TTY only; piped output is always raw)",
     .choices = CONFIG_CHOICES_BOOL, .runtime = 1},
    {.key = "show_reasoning", .env = "HAX_SHOW_REASONING",
     .desc = "Show reasoning/CoT deltas live (default off)",
     .choices = CONFIG_CHOICES_BOOL, .runtime = 1},
    {.key = "sort_models", .env = "HAX_SORT_MODELS", .def = "auto",
     .desc = "Sort the /model picker alphabetically; auto uses the provider's own default",
     .choices = CONFIG_CHOICES_TRISTATE, .runtime = 1},
    {.key = "context_limit", .env = "HAX_CONTEXT_LIMIT",
     .desc = "Manual context-window size for the % display; overrides auto-detect",
     .kind = CFG_SIZE, .runtime = 1},
    {.key = "display_width", .env = "HAX_DISPLAY_WIDTH",
     .desc = "Force content width in columns (default: terminal width, clamped)",
     .kind = CFG_INT, .runtime = 1},
    {.key = "notify", .env = "HAX_NOTIFY", .def = "auto",
     .desc = "Desktop-notification style: auto, bel, osc9, off (auto detects from the terminal)",
     .choices = "auto|bel|osc9|off", .runtime = 1},
    {.key = "theme", .env = "HAX_THEME", .def = "auto",
     .desc = "Color theme: auto, dark, light, ansi, off (auto detects from the terminal)",
     .choices = "auto|dark|light|ansi|off", .runtime = 1},

    /* behavior */
    {.key = "keep_awake", .env = "HAX_KEEP_AWAKE", .def = "1",
     .desc = "Inhibit idle system sleep while a turn is running (display may still blank)",
     .choices = CONFIG_CHOICES_BOOL, .runtime = 1},
    {.key = "compact.auto", .env = "HAX_COMPACT_AUTO", .def = "1",
     .desc = "Auto-summarize history when it nears the context window "
             "(manual /compact still works)",
     .choices = CONFIG_CHOICES_BOOL, .runtime = 1},
    {.key = "compact.threshold", .env = "HAX_COMPACT_THRESHOLD", .def = "85",
     .desc = "Auto-compact when context usage reaches this percent of the window",
     .kind = CFG_INT, .min = 1, .max = 100, .runtime = 1},
    {.key = "max_turns", .env = "HAX_MAX_TURNS",
     .desc = "Interactive: pause for confirmation after this many model round-trips per user turn",
     .kind = CFG_INT, .runtime = 1},

    /* model catalog */
    {.key = "catalog.url", .env = "HAX_CATALOG_URL", .def = "https://models.dev/api.json",
     .keep_empty = 1,
     .desc = "Model-metadata catalog endpoint (models.dev api.json shape); empty disables fetching"},
    {.key = "catalog.refresh", .env = "HAX_CATALOG_REFRESH", .def = "24h",
     .desc = "Re-fetch the cached model catalog when older than this; 0 disables fetching",
     .kind = CFG_DURATION},

    /* recording */
    {.key = "no_session", .env = "HAX_NO_SESSION", .choices = CONFIG_CHOICES_BOOL,
     .desc = "Disable session recording when set truthy"},
    {.key = "transcript", .env = "HAX_TRANSCRIPT", .keep_empty = 1,
     .desc = "Path to mirror the Ctrl-T transcript view; empty disables"},
    {.key = "trace", .env = "HAX_TRACE", .keep_empty = 1,
     .desc = "Path to a wire-level HTTP/SSE trace dump; empty disables"},

    /* tools */
    {.key = "tool_output_cap", .env = "HAX_TOOL_OUTPUT_CAP", .def = "50k",
     .desc = "Max bytes captured from a tool's output",
     .kind = CFG_SIZE, .runtime = 1},
    {.key = "bash.timeout", .env = "HAX_BASH_TIMEOUT", .def = "2m",
     .desc = "Default bash-tool command timeout; 0 disables",
     .kind = CFG_DURATION, .runtime = 1},
    {.key = "bash.timeout_max", .env = "HAX_BASH_TIMEOUT_MAX", .def = "30m",
     .desc = "Ceiling on the model's per-call bash timeout; 0 disables",
     .kind = CFG_DURATION, .runtime = 1},
    {.key = "bash.timeout_grace", .env = "HAX_BASH_TIMEOUT_GRACE", .def = "2s",
     .desc = "Grace window between SIGTERM and SIGKILL for bash commands; 0 skips",
     .kind = CFG_DURATION, .runtime = 1},
    {.key = "bash.shell", .env = "HAX_BASH_SHELL",
     .desc = "Shell for the bash tool, a $PATH name or path (default: bash, else sh)"},

    /* http transport */
    {.key = "http.max_retries", .env = "HAX_HTTP_MAX_RETRIES", .def = "4",
     .desc = "Additional retries for transient HTTP failures",
     .kind = CFG_INT, .max = 100, .runtime = 1},
    {.key = "http.retry_base", .env = "HAX_HTTP_RETRY_BASE", .def = "1s",
     .desc = "Base backoff between HTTP retries",
     .kind = CFG_DURATION, .min = 1 /* ms: must be positive */, .runtime = 1},
    {.key = "http.idle_timeout", .env = "HAX_HTTP_IDLE_TIMEOUT", .def = "10m",
     .desc = "Silence on a streaming response before giving up; 0 disables",
     .kind = CFG_DURATION, .runtime = 1},

    /* openai family (shared by the preset-based providers) */
    {.key = "openai.base_url", .env = "HAX_OPENAI_BASE_URL",
     .desc = "Base URL for the OpenAI-compatible endpoint"},
    {.key = "openai.api_key", .env = "HAX_OPENAI_API_KEY", .secret = 1,
     .desc = "Bearer token for OpenAI-family providers"},
    {.key = "openai.reasoning_format", .env = "HAX_OPENAI_REASONING_FORMAT",
     .desc = "Reasoning request dialect: flat or nested", .choices = "flat|nested"},
    {.key = "openai.reasoning_roundtrip", .env = "HAX_REASONING_ROUNDTRIP", .keep_empty = 1,
     .desc = "Replay reasoning text to the model (off/on, or a field name)"},
    {.key = "openai.send_cache_key", .env = "HAX_OPENAI_SEND_CACHE_KEY", .choices = CONFIG_CHOICES_TRISTATE,
     .desc = "Send a stable prompt_cache_key (prefix-cache hint); auto uses the provider default"},
    {.key = "openai.request_cost", .env = "HAX_OPENAI_REQUEST_COST", .choices = CONFIG_CHOICES_TRISTATE,
     .desc = "Request usage accounting (`usage: {include: true}`) for per-response cost; "
             "auto uses the provider default"},
    {.key = "provider_name", .env = "HAX_PROVIDER_NAME",
     .desc = "Display name for the provider in the banner"},

    /* anthropic family (shared by the anthropic + anthropic-compatible providers) */
    {.key = "anthropic.base_url", .env = "HAX_ANTHROPIC_BASE_URL",
     .desc = "Base URL for an Anthropic-compatible /v1 endpoint (anthropic-compatible)"},
    {.key = "anthropic.api_key", .env = "HAX_ANTHROPIC_API_KEY", .secret = 1,
     .desc = "x-api-key token for Anthropic-family providers"},
    {.key = "anthropic.max_tokens", .env = "HAX_ANTHROPIC_MAX_TOKENS", .def = "32000",
     .desc = "Max output tokens (thinking + text) per response",
     .kind = CFG_INT, .min = 1},
    {.key = "anthropic.thinking_mode", .env = "HAX_ANTHROPIC_THINKING_MODE",
     .desc = "Thinking mode: adaptive, budget, or off (default depends on the provider)",
     .choices = "adaptive|budget|off"},
    {.key = "anthropic.thinking_budget", .env = "HAX_ANTHROPIC_THINKING_BUDGET",
     .desc = "Budget-mode thinking tokens (default: max_tokens - 1)",
     .kind = CFG_INT, .min = 1},
    {.key = "anthropic.cache", .env = "HAX_ANTHROPIC_CACHE", .choices = CONFIG_CHOICES_TRISTATE,
     .desc = "Send prompt cache_control breakpoints; auto uses the provider default"},
    {.key = "anthropic.cache_ttl", .env = "HAX_ANTHROPIC_CACHE_TTL", .def = "1h",
     .desc = "Cache breakpoint TTL: 5m or 1h (1h suits an interactive agent's pauses)",
     .choices = "5m|1h"},
    {.key = "anthropic.version", .env = "HAX_ANTHROPIC_VERSION", .def = "2023-06-01",
     .desc = "anthropic-version request header value"},

    /* per-provider */
    {.key = "llamacpp.port", .env = "HAX_LLAMACPP_PORT", .def = "8080",
     .desc = "Port for the local llama-server (when openai.base_url is unset)",
     .kind = CFG_INT, .min = 1, .max = 65535},
    {.key = "openrouter.title", .env = "HAX_OPENROUTER_TITLE", .def = "hax", .keep_empty = 1,
     .desc = "X-Title header for OpenRouter attribution (empty disables)"},
    {.key = "openrouter.referer", .env = "HAX_OPENROUTER_REFERER",
     .def = "https://github.com/OleksandrChekhovskyi/hax", .keep_empty = 1,
     .desc = "HTTP-Referer header for OpenRouter attribution (empty disables)"},
    {.key = "mock.script", .env = "HAX_MOCK_SCRIPT",
     .desc = "Path to a mock-provider script (mock provider only)"},
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

const struct config_setting *config_setting_find(const char *key)
{
    return find_setting(key);
}

/* File tier (parsed config.json), state tier (parsed state.json — the
 * machine-local persisted overrides), and the session-only override tier
 * (a flat object keyed by canonical key). Each is NULL or a JSON object. */
static json_t *g_config;
static json_t *g_state;
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

/* Replace `*tier` with the JSON object parsed from `text`. Shared by the
 * config-file and state loaders — same grammar (object root, scalar leaves
 * normalized to strings) and return contract. */
static int load_tier(json_t **tier, const char *text)
{
    json_decref(*tier);
    *tier = NULL;
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
    *tier = root;
    return 0;
}

int config_load(const char *text)
{
    return load_tier(&g_config, text);
}

int config_load_state(const char *text)
{
    return load_tier(&g_state, text);
}

/* Read `path` into `*tier` via load_tier. Absent is silent (the tier is
 * optional); present-but-unusable (malformed, oversized, unreadable) is
 * ignored with a warning naming `what` — the user wrote the file and it
 * isn't being honored. Consumes `path` (frees it). */
static void init_tier_file(json_t **tier, char *path, const char *what)
{
    if (!path)
        return;
    size_t len;
    int truncated;
    errno = 0;
    char *data = slurp_file_capped(path, CONFIG_MAX_BYTES, &len, &truncated);
    if (data) {
        if (truncated)
            hax_warn("ignoring %s at %s: larger than the 1 MiB limit", what, path);
        else if (load_tier(tier, data) != 0)
            hax_warn("ignoring malformed %s at %s (expected a JSON object)", what, path);
        free(data);
    } else if (errno != ENOENT) {
        hax_warn("ignoring unreadable %s at %s: %s", what, path, strerror(errno));
    }
    free(path);
}

void config_init(void)
{
    init_tier_file(&g_config, xdg_hax_config_path("config.json"), "config");
    init_tier_file(&g_state, xdg_hax_state_path("state.json"), "state");
}

void config_free(void)
{
    json_decref(g_config);
    g_config = NULL;
    json_decref(g_state);
    g_state = NULL;
    json_decref(g_overrides);
    g_overrides = NULL;
}

/* Look up `key` in a JSON-object tier: the flat literal key first (the
 * dotted-key form), then a walk down nested objects on '.'. Returns the
 * leaf string, or NULL. Shared by the file and state tiers. */
static const char *obj_get(json_t *root, const char *key)
{
    if (!root)
        return NULL;
    json_t *v = json_object_get(root, key);
    if (!v) {
        json_t *cur = root;
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

/* Like obj_get, but returns the JSON *node* at `key` (object, array, or
 * scalar) rather than its string value — for callers that need to walk an
 * object's members. NULL when absent or a path segment isn't an object. */
static json_t *obj_get_node(json_t *root, const char *key)
{
    if (!root)
        return NULL;
    json_t *v = json_object_get(root, key); /* flat dotted key */
    if (v)
        return v;
    json_t *cur = root;
    const char *p = key;
    for (;;) {
        const char *dot = strchr(p, '.');
        if (!dot)
            return json_object_get(cur, p);
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

/* Add the name `k`[0..len) to *arr if absent (growing it), so the merged
 * result is deduplicated. */
static void add_object_key(char ***arr, size_t *n, size_t *cap, const char *k, size_t len)
{
    for (size_t i = 0; i < *n; i++)
        if (strlen((*arr)[i]) == len && strncmp((*arr)[i], k, len) == 0)
            return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = xrealloc(*arr, *cap * sizeof(**arr));
    }
    char *s = xmalloc(len + 1);
    memcpy(s, k, len);
    s[len] = '\0';
    (*arr)[(*n)++] = s;
}

/* Append the immediate member names of the object at `key` in `tier` to *arr.
 * Both authoring forms config.c accepts are honored, mirroring obj_get's read
 * path: the nested object ({"providers": {"<name>": {...}}}) and top-level
 * flat-dotted leaves ({"providers.<name>.<leaf>": "..."}, whose immediate
 * child is the segment after "<key>." up to the next '.'). Without the flat
 * scan a flat-defined provider would be readable but undiscoverable. Names
 * already present are skipped, so the merged result is deduplicated. */
static void collect_object_keys(json_t *tier, const char *key, char ***arr, size_t *n, size_t *cap)
{
    if (!json_is_object(tier))
        return;
    const char *k;
    json_t *v;
    json_t *obj = obj_get_node(tier, key);
    if (json_is_object(obj)) {
        json_object_foreach(obj, k, v) add_object_key(arr, n, cap, k, strlen(k));
    }
    size_t prefix = strlen(key);
    json_object_foreach(tier, k, v)
    {
        if (strncmp(k, key, prefix) != 0 || k[prefix] != '.')
            continue;
        const char *seg = k + prefix + 1;
        const char *dot = strchr(seg, '.');
        size_t len = dot ? (size_t)(dot - seg) : strlen(seg);
        if (len)
            add_object_key(arr, n, cap, seg, len);
    }
}

static const char *file_get(const char *key)
{
    return obj_get(g_config, key);
}

static const char *state_get(const char *key)
{
    return obj_get(g_state, key);
}

static const char *resolve(const char *key, int skip_empty);

/* "model" and "effort" are provider-bound: the runtime selectors
 * persist them together with the "provider" they were picked for, and a
 * config file pairing them with a provider means the same. A bound value
 * applies only while that provider is the active one — otherwise the tier is
 * skipped for these keys, so a one-off HAX_PROVIDER=mock doesn't inherit a
 * model saved for codex. A tier that records no provider is unbound (a bare
 * "model" in config.json applies everywhere), and the env/override tiers are
 * always honored — they are set explicitly for this run/session. */
static int binding_allows(json_t *tier, const char *key)
{
    if (strcmp(key, "model") != 0 && strcmp(key, "effort") != 0)
        return 1;
    const char *bound = obj_get(tier, "provider");
    if (!bound || !*bound)
        return 1;
    /* The verbatim read mirrors pick_provider: NULL/empty means no provider
     * resolved (auto-select will infer one), which no bound value can claim. */
    const char *active = resolve("provider", 0);
    return active && *active && strcmp(active, bound) == 0;
}

/* Walk the tiers: override (session) → environment → state → file →
 * registry default. With skip_empty, "" counts as unset at a tier and
 * resolution falls through — for settings whose grammar gives "" no meaning
 * (ports, durations), so a stray HAX_FOO= can't shadow a configured value.
 * Without it, values are returned verbatim, "" included. */
/* The CONFIG_VALUE_DEFAULT sentinel is honored at every tier (a literal
 * HAX_FOO="(default)" too): resolution stops there, shadowing lower tiers,
 * and yields the registry default — or NULL when the key declares none,
 * leaving the consumer's own default in charge. */
static const char *apply_sentinel(const char *v, const struct config_setting *s)
{
    if (v && strcmp(v, CONFIG_VALUE_DEFAULT) == 0)
        return s ? s->def : NULL;
    return v;
}

/* Resolve a value and optionally its winning tier. "default" covers both a
 * registry default and no value; a sentinel reports the tier that holds it. */
static const char *resolve_src(const char *key, int skip_empty, const char **src)
{
    const char *from = "default";
    const struct config_setting *s = find_setting(key);
    const char *v = NULL;
    const char *o = json_string_value(json_object_get(g_overrides, key));
    if (o && (!skip_empty || *o)) {
        v = apply_sentinel(o, s);
        from = "session";
    } else {
        const char *e = s ? getenv(s->env) : NULL;
        const char *sel = state_get(key);
        const char *f = file_get(key);
        if (e && (!skip_empty || *e)) {
            v = apply_sentinel(e, s);
            from = "env";
        } else if (sel && (!skip_empty || *sel) && binding_allows(g_state, key)) {
            v = apply_sentinel(sel, s);
            from = "state";
        } else if (f && (!skip_empty || *f) && binding_allows(g_config, key)) {
            v = apply_sentinel(f, s);
            from = "config";
        } else {
            v = s ? s->def : NULL;
        }
    }
    if (src)
        *src = from;
    return v;
}

static const char *resolve(const char *key, int skip_empty)
{
    return resolve_src(key, skip_empty, NULL);
}

/* Registered settings skip empty tiers unless explicitly marked otherwise;
 * unknown dynamic keys preserve them. */
static int config_skips_empty(const struct config_setting *s)
{
    return s && !s->keep_empty;
}

const char *config_str(const char *key)
{
    return resolve(key, config_skips_empty(find_setting(key)));
}

const char *config_source(const char *key)
{
    const char *src;
    resolve_src(key, config_skips_empty(find_setting(key)), &src);
    return src;
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

const json_t *config_json_node(const char *key)
{
    /* State over file mirrors resolve()'s tier order; no cross-tier merge —
     * a block is taken whole from the highest tier that defines it. */
    json_t *v = obj_get_node(g_state, key);
    return v ? v : obj_get_node(g_config, key);
}

size_t config_object_keys(const char *key, char ***out)
{
    char **arr = NULL;
    size_t n = 0, cap = 0;
    /* File and state tiers only: this enumerates user-authored structure
     * (config.json's providers.*, primarily), and the override tier is
     * flat-keyed so it holds no nested objects to walk. */
    collect_object_keys(g_config, key, &arr, &n, &cap);
    collect_object_keys(g_state, key, &arr, &n, &cap);
    *out = arr;
    return n;
}

/* Typed getters skip empty values and fall back to registry defaults on parse
 * or bounds failures. Bounds use the setting's native unit; zero is unbounded. */
static int in_bounds(const struct config_setting *s, long v)
{
    if (!s)
        return 1;
    if (s->min && v < s->min)
        return 0;
    if (s->max && v > s->max)
        return 0;
    return 1;
}

int config_int(const char *key)
{
    const struct config_setting *s = find_setting(key);
    int v;
    /* Negative values are invalid: every integer setting is a count or width.
     * Bounds fail the same way, so consumers need not re-check them. */
    if (parse_int(resolve(key, 1), &v) && v >= 0 && in_bounds(s, v))
        return v;
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

int config_value_valid(const struct config_setting *s, const char *val)
{
    if (!s || !val)
        return 0;
    if (!s->choices) {
        switch (s->kind) {
        case CFG_INT: {
            int v;
            return parse_int(val, &v) && v >= 0 && in_bounds(s, v);
        }
        case CFG_SIZE: {
            long v = parse_size(val);
            return v > 0 && in_bounds(s, v);
        }
        case CFG_DURATION: {
            long v = parse_duration_ms(val);
            return v >= 0 && in_bounds(s, v);
        }
        case CFG_STRING:
            return 1;
        }
        return 1;
    }
    /* Boolean choices accept the full config_bool grammar. Tri-state adds an
     * "auto" literal on top (unset/auto defers to the consumer's own default),
     * so on/off keep the same lenient spelling whether or not auto is present. */
    if (strcmp(s->choices, CONFIG_CHOICES_BOOL) == 0)
        return parse_bool(val) >= 0;
    if (strcmp(s->choices, CONFIG_CHOICES_TRISTATE) == 0)
        return strcasecmp(val, "auto") == 0 || parse_bool(val) >= 0;
    const char *p = s->choices;
    size_t vlen = strlen(val);
    for (;;) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (n == vlen && strncasecmp(p, val, n) == 0)
            return 1;
        if (!bar)
            return 0;
        p = bar + 1;
    }
}

void config_value_hint(const struct config_setting *s, char *buf, size_t n)
{
    if (n == 0)
        return;
    buf[0] = '\0';
    if (!s)
        return;
    if (s->choices) {
        snprintf(buf, n, "%s", s->choices);
        return;
    }
    switch (s->kind) {
    case CFG_INT:
        if (s->min && s->max)
            snprintf(buf, n, "a whole number from %ld to %ld", s->min, s->max);
        else if (s->max)
            snprintf(buf, n, "a whole number up to %ld", s->max);
        else if (s->min)
            snprintf(buf, n, "a whole number of at least %ld", s->min);
        else
            snprintf(buf, n, "a whole number");
        break;
    case CFG_SIZE:
        snprintf(buf, n, "a size like 64k or 1M");
        break;
    case CFG_DURATION:
        snprintf(buf, n, "a duration like 2s or 500ms");
        break;
    case CFG_STRING:
        break;
    }
}

char *config_value_canonical(const struct config_setting *s, const char *val)
{
    /* Only strict enums need this; booleans and free-form values are parsed
     * directly at their point of use. */
    if (!s || !val || !s->choices || strcmp(s->choices, CONFIG_CHOICES_BOOL) == 0)
        return NULL;
    const char *p = s->choices;
    size_t vlen = strlen(val);
    for (;;) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        if (n == vlen && strncasecmp(p, val, n) == 0) {
            char *out = xmalloc(n + 1);
            memcpy(out, p, n);
            out[n] = '\0';
            return out;
        }
        if (!bar)
            return NULL;
        p = bar + 1;
    }
}

long config_size(const char *key)
{
    const struct config_setting *s = find_setting(key);
    long v = parse_size(resolve(key, 1)); /* NULL-safe; 0 = unset or invalid */
    if (v > 0 && in_bounds(s, v))
        return v;
    return s ? parse_size(s->def) : 0;
}

long config_duration_ms(const char *key)
{
    const struct config_setting *s = find_setting(key);
    long v = parse_duration_ms(resolve(key, 1)); /* NULL-safe; -1 = unset or invalid */
    if (v >= 0 && in_bounds(s, v))
        return v;
    v = s ? parse_duration_ms(s->def) : -1;
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

struct config_override_state {
    json_t *overrides; /* deep copy of g_overrides at snapshot time, or NULL */
};

struct config_override_state *config_override_snapshot(void)
{
    struct config_override_state *s = xmalloc(sizeof(*s));
    s->overrides = g_overrides ? json_deep_copy(g_overrides) : NULL;
    return s;
}

void config_override_restore(struct config_override_state *snap)
{
    if (!snap)
        return;
    json_decref(g_overrides);
    g_overrides = snap->overrides; /* transfer ownership back to the live tier */
    free(snap);
}

void config_override_state_free(struct config_override_state *snap)
{
    if (!snap)
        return;
    json_decref(snap->overrides);
    free(snap);
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

/* Persist `key` = `val` into the JSON object at `path`, swapping the result
 * into `*tier`. Consumes `path` (frees it). Mutate a copy and swap it in only
 * after the write succeeds, so a failed write can't leave the in-memory tier
 * claiming a value the disk never saw. Shared by the config-file and state
 * writers. */
static int persist_tier(json_t **tier, char *path, const char *key, const char *val)
{
    if (!path)
        return -1;
    json_t *next = *tier ? json_deep_copy(*tier) : json_object();
    if (!next) {
        free(path);
        return -1;
    }
    /* A hand-written flat dotted key ({"openai.base_url": ...}) shadows
     * the nested path in obj_get, so drop it — otherwise it would mask
     * the value persisted below (or survive a deletion). Rewriting the
     * file also normalizes it to nested form as a side effect. */
    json_object_del(next, key);
    set_nested(next, key, val);

    int rc = write_json_atomic(path, next);
    free(path);
    if (rc != 0) {
        json_decref(next);
        return -1;
    }
    json_decref(*tier);
    *tier = next;
    return 0;
}

int config_persist(const char *key, const char *val)
{
    return persist_tier(&g_config, xdg_hax_config_path("config.json"), key, val);
}

int config_persist_state(const char *key, const char *val)
{
    return persist_tier(&g_state, xdg_hax_state_path("state.json"), key, val);
}

int config_persist_selection(const char *provider, const char *model, const char *effort)
{
    if (!provider || !*provider)
        return -1;
    char *path = xdg_hax_state_path("state.json");
    if (!path)
        return -1;
    /* Mutate a copy and swap after the write succeeds, like persist_tier. */
    json_t *next = g_state ? json_deep_copy(g_state) : json_object();
    if (!next) {
        free(path);
        return -1;
    }

    /* Re-pinning a different provider orphans the members not picked this
     * time — they belong to the old provider — so they reset to the sentinel
     * (the new provider's own default beats a stale lower-tier value). On an
     * unchanged provider an unpicked member keeps its stored value: an
     * /effort pick must not wipe a saved model. */
    const char *old = obj_get(g_state, "provider");
    int repin = !old || strcmp(old, provider) != 0;
    /* An explicit selection commit replaces a persisted preset stance:
     * without this, the preset would re-apply next launch as an override
     * and shadow the very selection being committed here. The session-side
     * counterpart (clearing the "preset" and "system_prompt" overrides)
     * lives in the selectors' commit path. */
    json_object_del(next, "preset");
    json_object_set_new(next, "provider", json_string(provider));
    if (model || repin)
        json_object_set_new(next, "model", json_string(model ? model : CONFIG_VALUE_DEFAULT));
    if (effort || repin)
        json_object_set_new(next, "effort", json_string(effort ? effort : CONFIG_VALUE_DEFAULT));

    int rc = write_json_atomic(path, next);
    free(path);
    if (rc != 0) {
        json_decref(next);
        return -1;
    }
    json_decref(g_state);
    g_state = next;
    return 0;
}

/* The presets.<name> object for `name`, or NULL. Looked up as a *literal*
 * member of each tier's "presets" object — never through the dotted-path
 * grammar, which would misread a user-chosen name like "review.v2" as
 * nesting and make an enumerated preset unappliable. State over file per
 * name, matching config_object_keys' merged enumeration. The flat-authored
 * top-level form ({"presets.<name>": {...}}) is kept working via the dotted
 * fallback, which by construction only resolves dot-free names. */
static const json_t *preset_node(const char *name)
{
    json_t *const tiers[] = {g_state, g_config};
    for (size_t i = 0; i < sizeof(tiers) / sizeof(*tiers); i++) {
        json_t *presets = obj_get_node(tiers[i], "presets");
        json_t *obj = json_is_object(presets) ? json_object_get(presets, name) : NULL;
        if (json_is_object(obj))
            return obj;
    }
    char *key = xasprintf("presets.%s", name);
    const json_t *obj = config_json_node(key);
    free(key);
    return json_is_object(obj) ? obj : NULL;
}

/* The presettable keys. Deliberately narrow: a preset must be fully honored
 * whenever it is applied — startup or mid-session — so only per-request
 * settings qualify. Construction-bound settings (openai.base_url, api keys,
 * provider_name) belong in a providers.<name> block the preset points at;
 * startup-latched behavior (context stripping, session recording) belongs
 * to the --bare / --no-session flags. */
static const char *const PRESET_KEYS[] = {"provider", "model", "effort", "system_prompt"};

/* Structural validation shared by apply and enumeration: the whole block
 * checks out or the preset is unusable — a typo'd member must not leave the
 * session running half a preset, and the enumerators must not advertise a
 * definition that would then fail. "description" is reserved metadata
 * (picker/prompt listings), not a setting. Load-time normalization already
 * turned scalar members into strings, so a non-string here is a nested
 * object or an array — not expressible as an override. Non-mutating;
 * *err (when non-NULL) receives a malloc'd reason on failure. */
static int preset_validate(const json_t *obj, const char *name, char **err)
{
    const char *k;
    json_t *v;
    json_object_foreach((json_t *)obj, k, v)
    {
        /* "description" is exempt from the allowed-keys check (reserved
         * metadata, not a setting) but not from the scalar check below: a
         * structured value would silently read back as "no description" —
         * exactly where descriptions matter, guiding preset choice. */
        if (strcmp(k, "description") != 0) {
            int allowed = 0;
            for (size_t i = 0; i < sizeof(PRESET_KEYS) / sizeof(*PRESET_KEYS) && !allowed; i++)
                allowed = strcmp(k, PRESET_KEYS[i]) == 0;
            if (!allowed) {
                if (err)
                    *err = xasprintf(
                        "preset '%s': '%s' is not presettable (allowed: provider, model, "
                        "effort, system_prompt); endpoint settings belong in a "
                        "providers.<name> block, context/recording in the --bare/--no-session "
                        "flags",
                        name, k);
                return -1;
            }
        }
        if (!json_is_string(v)) {
            if (err)
                *err = xasprintf("preset '%s': '%s' must be a scalar", name, k);
            return -1;
        }
    }
    const char *prov = json_string_value(json_object_get((json_t *)obj, "provider"));
    if (!prov || !*prov) {
        if (err)
            *err = xasprintf("preset '%s' must name a provider", name);
        return -1;
    }
    return 0;
}

int config_preset_apply(const char *name, char **err)
{
    if (err)
        *err = NULL;
    const json_t *obj = preset_node(name);
    if (!obj) {
        if (err)
            *err = xasprintf("unknown preset '%s' (define a presets.%s block in config.json)", name,
                             name);
        return -1;
    }
    if (preset_validate(obj, name, err) != 0)
        return -1;

    /* A preset is a whole selection, so applying one replaces the previous
     * preset instead of composing with it: unnamed model/effort reset to the
     * sentinel — the named provider's own default applies, shadowing stale
     * lower-tier values exactly like a /provider re-pin (see
     * config_persist_selection) — and an unnamed system_prompt clears the
     * override outright. system_prompt is the one preset-owned override
     * (nothing else writes it), and it isn't provider-bound, so falling back
     * to normal resolution (a user's configured prompt) is right where the
     * sentinel would wrongly force the built-in. */
    const char *m = json_string_value(json_object_get((json_t *)obj, "model"));
    const char *e = json_string_value(json_object_get((json_t *)obj, "effort"));
    config_set_override("provider", json_string_value(json_object_get((json_t *)obj, "provider")));
    config_set_override("model", m ? m : CONFIG_VALUE_DEFAULT);
    config_set_override("effort", e ? e : CONFIG_VALUE_DEFAULT);
    config_set_override("system_prompt",
                        json_string_value(json_object_get((json_t *)obj, "system_prompt")));
    /* Record the active stance under the "preset" key — what the banner and
     * /session read, so a preset that swapped the system prompt is never
     * invisibly in effect. Cleared when an explicit selection commit exits
     * the stance (commit_selection). */
    config_set_override("preset", name);
    return 0;
}

const char *config_preset_description(const char *name)
{
    const json_t *obj = preset_node(name);
    if (!obj)
        return NULL;
    return json_string_value(json_object_get(obj, "description"));
}

const char *config_preset_provider(const char *name)
{
    const json_t *obj = preset_node(name);
    if (!obj)
        return NULL;
    return json_string_value(json_object_get(obj, "provider"));
}

size_t config_preset_names(char ***out)
{
    /* Filter through the same resolution + validation that apply uses, so
     * "enumerated ⊆ appliable" holds by construction — the /preset picker
     * and the system prompt's listing must never advertise a preset that
     * then fails, whether unresolvable (a name spelled only as fully-flat
     * leaves, which preset_node cannot assemble) or structurally invalid
     * (missing provider, unknown member). Skipped definitions are still
     * user-authored config that isn't being honored, so they warn — once
     * per process, since enumeration runs on every prompt rebuild. */
    static int warned;
    char **names = NULL;
    size_t n = config_object_keys("presets", &names);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        const json_t *obj = preset_node(names[i]);
        char *err = NULL;
        if (obj && preset_validate(obj, names[i], warned ? NULL : &err) == 0) {
            names[kept++] = names[i];
            continue;
        }
        if (!warned) {
            if (err)
                hax_warn("%s — ignoring it", err);
            else if (!obj)
                hax_warn("preset '%s' is not an object (define a presets.%s block) — "
                         "ignoring it",
                         names[i], names[i]);
        }
        free(err);
        free(names[i]);
    }
    warned = 1;
    *out = names;
    return kept;
}
