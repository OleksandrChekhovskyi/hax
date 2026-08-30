/* SPDX-License-Identifier: MIT */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <jansson.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "diag.h"
#include "provider.h"
#include "util.h"
#include "system/fs.h"
#include "system/path.h"
#include "text/utf8_sanitize.h"

/* Canonical keys, env bindings, defaults, and /config metadata. Row order is
 * user-visible. Runtime settings must be read live or refreshed after edits;
 * numeric bounds are enforced by the typed getters. */
// clang-format off
static const struct config_setting REGISTRY[] = {
    /* selection */
    {.key = "preset", .env_var = "HAX_PRESET", .keep_empty = 1,
     .description = "Preset from presets.<name> to apply at startup; empty disables"},
    {.key = "provider", .env_var = "HAX_PROVIDER", .keep_empty = 1,
     .description = "Provider id; /provider shows the available choices"},
    {.key = "model", .env_var = "HAX_MODEL", .keep_empty = 1,
     .description = "Model id (provider-specific; some auto-fill or require it)"},
    {.key = "effort", .env_var = "HAX_EFFORT", .keep_empty = 1,
     .description = "Reasoning effort (provider-specific); empty omits it"},
    {.key = "system_prompt", .env_var = "HAX_SYSTEM_PROMPT", .keep_empty = 1,
     .description = "Replace the built-in base prompt (context sections still follow); @path "
                    "reads a file; (none) sends no system message at all"},
    {.key = "system_prompt_append", .env_var = "HAX_SYSTEM_PROMPT_APPEND", .keep_empty = 1,
     .description = "Text appended after the base system prompt; @path reads a file"},
    {.key = "no_env", .env_var = "HAX_NO_ENV", .choices = CONFIG_CHOICES_BOOL,
     .description = "Skip the Environment section in the system prompt"},
    {.key = "no_agents_md", .env_var = "HAX_NO_AGENTS_MD", .choices = CONFIG_CHOICES_BOOL,
     .description = "Skip AGENTS.md project instructions in the system prompt"},
    {.key = "no_skills", .env_var = "HAX_NO_SKILLS", .choices = CONFIG_CHOICES_BOOL,
     .description = "Skip the skills listing in the system prompt"},
    {.key = "no_subagents", .env_var = "HAX_NO_SUBAGENTS", .choices = CONFIG_CHOICES_BOOL,
     .description = "Skip the subagents section in the system prompt"},
    {.key = "no_tasks", .env_var = "HAX_NO_TASKS", .choices = CONFIG_CHOICES_BOOL,
     .description = "Disable background tasks: bash timeouts kill instead of detaching, and the "
                    "task tools are not offered"},

    /* display */
    {.key = "markdown", .env_var = "HAX_MARKDOWN", .default_value = "1",
     .description = "Render Markdown in the terminal (TTY only; piped output is always raw)",
     .choices = CONFIG_CHOICES_BOOL, .editable = 1},
    {.key = "show_reasoning", .env_var = "HAX_SHOW_REASONING",
     .description = "Show reasoning/CoT deltas live (default off)",
     .choices = CONFIG_CHOICES_BOOL, .editable = 1},
    {.key = "sort_models", .env_var = "HAX_SORT_MODELS", .default_value = "auto",
     .description = "Sort the /model picker newest-first; auto uses the provider's own default",
     .choices = CONFIG_CHOICES_TRISTATE, .editable = 1},
    {.key = "context_limit", .env_var = "HAX_CONTEXT_LIMIT",
     .description = "Manual context-window size for the % display; overrides auto-detect",
     .kind = CONFIG_KIND_TOKENS, .editable = 1},
    {.key = "display_width", .env_var = "HAX_DISPLAY_WIDTH", .default_value = "auto",
     .description = "Content width: auto uses full width through 110 columns and 100 beyond that; "
                    "terminal always uses full width; a number sets an exact width",
     .choices = "auto|terminal", .example = "100", .kind = CONFIG_KIND_INT, .min = 20,
     .editable = 1},
    {.key = "notify", .env_var = "HAX_NOTIFY", .default_value = "auto",
     .description = "Desktop-notification style: auto, bel, osc9, off "
                    "(auto detects from the terminal)",
     .choices = "auto|bel|osc9|off", .editable = 1},
    {.key = "theme", .env_var = "HAX_THEME", .default_value = "auto",
     .description = "Color theme: auto, dark, light, ansi, off (auto detects from the terminal)",
     .choices = "auto|dark|light|ansi|off", .editable = 1},
    {.key = "tint", .env_var = "HAX_TINT", .default_value = "teal",
     .description = "Identity tint for model output; an active preset's own tint wins until set "
                    "here. Ignored by the ansi and off themes",
     .choices = "teal|violet|rose|sage", .editable = 1},

    /* behavior */
    {.key = "keep_awake", .env_var = "HAX_KEEP_AWAKE", .default_value = "1",
     .description = "Inhibit idle system sleep while a turn is running (display may still blank)",
     .choices = CONFIG_CHOICES_BOOL, .editable = 1},
    {.key = "compact.auto", .env_var = "HAX_COMPACT_AUTO", .default_value = "1",
     .description = "Auto-summarize history when it nears the context window "
                    "(manual /compact still works)",
     .choices = CONFIG_CHOICES_BOOL, .editable = 1},
    {.key = "compact.threshold", .env_var = "HAX_COMPACT_THRESHOLD", .default_value = "85",
     .description = "Auto-compact when context usage reaches this percent of the window",
     .kind = CONFIG_KIND_INT, .min = 1, .max = 100, .editable = 1},
    {.key = "max_turns", .env_var = "HAX_MAX_TURNS",
     .description = "Interactive: pause for confirmation after this many model round-trips per "
                    "user turn",
     .kind = CONFIG_KIND_INT, .editable = 1},

    /* model catalog */
    {.key = "catalog.url", .env_var = "HAX_CATALOG_URL",
     .default_value = "https://models.dev/api.json", .keep_empty = 1,
     .description = "Model-metadata catalog endpoint (models.dev api.json shape); empty disables "
                    "fetching"},
    {.key = "catalog.refresh", .env_var = "HAX_CATALOG_REFRESH", .default_value = "24h",
     .description = "Re-fetch the cached model catalog when older than this; 0 disables fetching",
     .kind = CONFIG_KIND_DURATION},

    /* recording */
    {.key = "no_session", .env_var = "HAX_NO_SESSION", .default_value = "auto",
     .description = "Skip recording conversations and typed prompts; auto skips both for dev "
                    "providers (mock)",
     .choices = CONFIG_CHOICES_TRISTATE},
    {.key = "session_retention_days", .env_var = "HAX_SESSION_RETENTION_DAYS",
     .default_value = "30",
     .description = "Delete sessions after this many inactive days; 0 disables pruning",
     .kind = CONFIG_KIND_INT, .max = 36500},
    {.key = "transcript", .env_var = "HAX_TRANSCRIPT", .keep_empty = 1,
     .description = "Path to mirror the Ctrl-T transcript view; empty disables"},
    {.key = "trace", .env_var = "HAX_TRACE", .keep_empty = 1,
     .description = "Path to a wire-level HTTP/SSE trace dump; empty disables"},

    /* tools */
    {.key = "image_input", .env_var = "HAX_IMAGE_INPUT", .default_value = "auto",
     .description = "Let the model view images via the read tool; auto detects per provider/model",
     .choices = CONFIG_CHOICES_TRISTATE, .editable = 1},
    {.key = "tool_output_cap", .env_var = "HAX_TOOL_OUTPUT_CAP", .default_value = "50k",
     .description = "Max bytes captured from a tool's output",
     .kind = CONFIG_KIND_SIZE, .editable = 1},
    {.key = "bash.timeout", .env_var = "HAX_BASH_TIMEOUT", .default_value = "2m",
     .description = "Default bash-tool command timeout: the command detaches into a background "
                    "task (kills when tasks are disabled); 0 disables",
     .kind = CONFIG_KIND_DURATION, .editable = 1},
    {.key = "bash.timeout_max", .env_var = "HAX_BASH_TIMEOUT_MAX", .default_value = "30m",
     .description = "Ceiling on the model's per-call bash timeout; 0 disables",
     .kind = CONFIG_KIND_DURATION, .editable = 1},
    {.key = "bash.timeout_grace", .env_var = "HAX_BASH_TIMEOUT_GRACE", .default_value = "2s",
     .description = "Grace window between SIGTERM and SIGKILL for bash commands; 0 skips",
     .kind = CONFIG_KIND_DURATION, .max = 300000, .editable = 1},
    {.key = "bash.background_yield", .env_var = "HAX_BASH_BACKGROUND_YIELD",
     .default_value = "5s",
     .description = "Initial output window before an explicitly backgrounded bash command "
                    "detaches into a task",
     .kind = CONFIG_KIND_DURATION, .editable = 1},
    {.key = "bash.shell", .env_var = "HAX_BASH_SHELL",
     .description = "Shell for the bash tool, a $PATH name or path (default: bash, else sh)"},
    {.key = "task.wait_timeout", .env_var = "HAX_TASK_WAIT_TIMEOUT", .default_value = "10m",
     .description = "Default task_wait timeout when the model omits one (kill waits default "
                    "to immediate)",
     .kind = CONFIG_KIND_DURATION, .editable = 1},
    {.key = "task.max_running", .env_var = "HAX_TASK_MAX_RUNNING", .default_value = "32",
     .description = "Maximum concurrently running background tasks",
     .kind = CONFIG_KIND_INT, .min = 1, .max = 64, .editable = 1},

    /* http transport */
    {.key = "http.max_retries", .env_var = "HAX_HTTP_MAX_RETRIES", .default_value = "4",
     .description = "Additional retries for transient HTTP failures",
     .kind = CONFIG_KIND_INT, .max = 100, .editable = 1},
    {.key = "http.retry_base", .env_var = "HAX_HTTP_RETRY_BASE", .default_value = "1s",
     .description = "Base backoff between HTTP retries",
     .kind = CONFIG_KIND_DURATION, .min = 1 /* ms: must be positive */, .editable = 1},
    {.key = "http.idle_timeout", .env_var = "HAX_HTTP_IDLE_TIMEOUT", .default_value = "10m",
     .description = "Silence on a streaming response before giving up; 0 disables",
     .kind = CONFIG_KIND_DURATION, .editable = 1},

    /* openai-compatible (the shipped generic-endpoint provider; the env vars are aliases into
     * its providers.* block so a compatible endpoint stays one-shot configurable) */
    {.key = "providers.openai-compatible.base_url", .env_var = "HAX_OPENAI_BASE_URL",
     .description = "Base URL of the OpenAI-compatible endpoint"},
    {.key = "providers.openai-compatible.api_key", .env_var = "HAX_OPENAI_API_KEY", .secret = 1,
     .description = "Bearer token for the OpenAI-compatible endpoint"},
    {.key = "providers.openai-compatible.display_name", .env_var = "HAX_OPENAI_DISPLAY_NAME",
     .description = "Display name for the provider in the banner and picker"},
    {.key = "providers.openai-compatible.api", .env_var = "HAX_OPENAI_API",
     .description = "Request protocol: chat (Chat Completions) or responses",
     .choices = "chat|responses"},
    {.key = "providers.openai-compatible.reasoning_format",
     .env_var = "HAX_OPENAI_REASONING_FORMAT",
     .description = "Reasoning request dialect: flat or nested", .choices = "flat|nested"},
    {.key = "providers.openai-compatible.reasoning_roundtrip",
     .env_var = "HAX_REASONING_ROUNDTRIP", .keep_empty = 1,
     .description = "Replay reasoning text to the model (off/on, or a field name)"},
    {.key = "providers.openai-compatible.send_cache_key", .env_var = "HAX_OPENAI_SEND_CACHE_KEY",
     .choices = CONFIG_CHOICES_TRISTATE,
     .description = "Send a stable prompt_cache_key (prefix-cache hint); auto uses the provider "
                    "default"},
    {.key = "providers.openai-compatible.request_cost", .env_var = "HAX_OPENAI_REQUEST_COST",
     .choices = CONFIG_CHOICES_TRISTATE,
     .description = "Request usage accounting (`usage: {include: true}`) for per-response cost; "
                    "auto uses the provider default"},
    {.key = "providers.openai-compatible.cache", .env_var = "HAX_OPENAI_CACHE",
     .choices = CONFIG_CHOICES_TRISTATE,
     .description = "Send prompt cache_control breakpoints (routers fronting Anthropic models, "
                    "which cache only on request); auto uses the provider default"},
    {.key = "providers.openai-compatible.cache_ttl", .env_var = "HAX_OPENAI_CACHE_TTL",
     .description = "Cache breakpoint TTL: 5m or 1h (default 1h, suiting an interactive agent's "
                    "pauses)",
     .choices = "5m|1h"},

    /* anthropic-compatible (same scheme for the generic Messages provider) */
    {.key = "providers.anthropic-compatible.base_url", .env_var = "HAX_ANTHROPIC_BASE_URL",
     .description = "Base URL of the Anthropic-compatible /v1 endpoint"},
    {.key = "providers.anthropic-compatible.api_key", .env_var = "HAX_ANTHROPIC_API_KEY",
     .secret = 1,
     .description = "x-api-key token for the Anthropic-compatible endpoint"},
    {.key = "providers.anthropic-compatible.display_name", .env_var = "HAX_ANTHROPIC_DISPLAY_NAME",
     .description = "Display name for the provider in the banner and picker"},
    /* Unset follows model metadata; a registry default would make /config report a value the
     * request does not necessarily use. */
    {.key = "providers.anthropic-compatible.max_tokens", .env_var = "HAX_ANTHROPIC_MAX_TOKENS",
     .description = "Max output tokens (thinking + text) per response; unset follows the model's "
                    "own cap",
     .kind = CONFIG_KIND_INT, .min = 1},
    {.key = "providers.anthropic-compatible.thinking_mode",
     .env_var = "HAX_ANTHROPIC_THINKING_MODE",
     .description = "Thinking mode: adaptive, budget, or off",
     .choices = "adaptive|budget|off"},
    {.key = "providers.anthropic-compatible.thinking_budget",
     .env_var = "HAX_ANTHROPIC_THINKING_BUDGET",
     .description = "Budget-mode thinking tokens (default: max_tokens - 1)",
     .kind = CONFIG_KIND_INT, .min = 1},
    {.key = "providers.anthropic-compatible.cache", .env_var = "HAX_ANTHROPIC_CACHE",
     .choices = CONFIG_CHOICES_TRISTATE,
     .description = "Send prompt cache_control breakpoints; auto uses the provider default"},
    {.key = "providers.anthropic-compatible.cache_ttl", .env_var = "HAX_ANTHROPIC_CACHE_TTL",
     .description = "Cache breakpoint TTL: 5m or 1h (default 1h, suiting an interactive agent's "
                    "pauses)",
     .choices = "5m|1h"},
    {.key = "providers.anthropic-compatible.version", .env_var = "HAX_ANTHROPIC_VERSION",
     .description = "anthropic-version request header value (default: 2023-06-01)"},

    /* per-provider */
    {.key = "providers.llamacpp.base_url", .env_var = "HAX_LLAMACPP_BASE_URL",
     .description = "Full llama-server base URL; overrides the port setting"},
    {.key = "providers.llamacpp.api_key", .env_var = "HAX_LLAMACPP_API_KEY", .secret = 1,
     .description = "Bearer token when llama-server runs with --api-key"},
    {.key = "providers.llamacpp.port", .env_var = "HAX_LLAMACPP_PORT", .default_value = "8080",
     .description = "Port for the local llama-server (when base_url is unset)",
     .kind = CONFIG_KIND_INT, .min = 1, .max = 65535},
    {.key = "providers.openrouter.title", .env_var = "HAX_OPENROUTER_TITLE",
     .default_value = "hax", .keep_empty = 1,
     .description = "X-Title header for OpenRouter attribution (empty disables)"},
    {.key = "providers.openrouter.referer", .env_var = "HAX_OPENROUTER_REFERER",
     .default_value = "https://usehax.dev", .keep_empty = 1,
     .description = "HTTP-Referer header for OpenRouter attribution (empty disables)"},
    {.key = "providers.mock.script", .env_var = "HAX_MOCK_SCRIPT",
     .description = "Path to a mock-provider script (mock provider only)"},
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

const struct config_setting *config_settings(size_t *count)
{
    *count = sizeof(REGISTRY) / sizeof(REGISTRY[0]);
    return REGISTRY;
}

const struct config_setting *config_setting_find(const char *key)
{
    return find_setting(key);
}

/* A string view of a numeric or boolean scalar, materialized on first string read. */
struct scalar_string {
    const json_t *node;
    char *text;
};

struct config_store {
    json_t *file;
    json_t *state;
    json_t *conversation;
    json_t *run;
    /* Prevent writes from replacing config.json content that was never loaded. */
    int file_unusable;
    /* Tiers are kept verbatim, so string reads coerce numbers and booleans here on first use:
     * config_str callers borrow until the next config mutation, so a transient buffer would
     * not do. Keyed by node; cleared whenever a file/state tier is replaced, which frees the
     * nodes (a recycled allocation must not resurrect a stale entry). */
    struct scalar_string *scalar_strings;
    size_t n_scalar_strings;
    size_t scalar_strings_capacity;
    /* Preset names whose defect a failed apply already surfaced, so enumeration does not
     * repeat the same message; the once-per-configuration latch for its other warnings. */
    char **reported_presets;
    size_t n_reported_presets;
    int preset_warnings_emitted;
};

static struct config_store store;

#define CONFIG_MAX_BYTES (1024 * 1024)

static void scalar_cache_clear(void)
{
    for (size_t i = 0; i < store.n_scalar_strings; i++)
        free(store.scalar_strings[i].text);
    free(store.scalar_strings);
    store.scalar_strings = NULL;
    store.n_scalar_strings = 0;
    store.scalar_strings_capacity = 0;
}

/* Read a JSON value as a string setting, coercing a number or boolean; other types are
 * not string-readable and return NULL. */
static const char *scalar_as_string(const json_t *value)
{
    if (!value || json_is_string(value))
        return json_string_value(value);
    if (!json_is_number(value) && !json_is_boolean(value))
        return NULL;

    for (size_t i = 0; i < store.n_scalar_strings; i++) {
        if (store.scalar_strings[i].node == value)
            return store.scalar_strings[i].text;
    }

    char buffer[32];
    if (json_is_integer(value))
        snprintf(buffer, sizeof buffer, "%lld", (long long)json_integer_value(value));
    else if (json_is_real(value))
        snprintf(buffer, sizeof buffer, "%g", json_real_value(value));
    else
        snprintf(buffer, sizeof buffer, "%s", json_is_true(value) ? "1" : "0");

    if (store.n_scalar_strings == store.scalar_strings_capacity) {
        store.scalar_strings_capacity =
            store.scalar_strings_capacity ? store.scalar_strings_capacity * 2 : 8;
        store.scalar_strings = xrealloc(store.scalar_strings, store.scalar_strings_capacity *
                                                                  sizeof(*store.scalar_strings));
    }
    struct scalar_string *entry = &store.scalar_strings[store.n_scalar_strings++];
    entry->node = value;
    entry->text = xstrdup(buffer);
    return entry->text;
}

static int load_tier(json_t **tier, const char *text)
{
    scalar_cache_clear();
    json_decref(*tier);
    *tier = NULL;

    while (text && isspace((unsigned char)*text))
        text++;
    if (!text || !*text)
        return 0;

    json_t *root = json_loads(text, 0, NULL);
    if (!json_is_object(root)) {
        json_decref(root);
        return -1;
    }

    *tier = root;
    return 0;
}

int config_load(const char *text)
{
    store.file_unusable = 0;
    return load_tier(&store.file, text);
}

int config_load_state(const char *text)
{
    return load_tier(&store.state, text);
}

/* `path` is consumed. Missing files are valid empty tiers. */
static int load_tier_file(json_t **tier, char *path, const char *label)
{
    if (!path)
        return 0;

    int unusable = 0;
    int truncated;
    errno = 0;
    char *text = slurp_file_capped(path, CONFIG_MAX_BYTES, NULL, &truncated);
    if (text) {
        if (truncated) {
            hax_warn("ignoring %s at %s: larger than the 1 MiB limit", label, path);
            unusable = 1;
        } else if (load_tier(tier, text) != 0) {
            hax_warn("ignoring malformed %s at %s (expected a JSON object)", label, path);
            unusable = 1;
        }
        free(text);
    } else if (errno != ENOENT) {
        hax_warn("ignoring unreadable %s at %s: %s", label, path, strerror(errno));
        unusable = 1;
    }

    free(path);
    return unusable ? -1 : 0;
}

void config_init(void)
{
    store.file_unusable =
        load_tier_file(&store.file, xdg_hax_config_path("config.json"), "config") != 0;
    load_tier_file(&store.state, xdg_hax_state_path("state.json"), "state");
}

/* Defined below with the other preset internals. */
static void free_reported_presets(void);

void config_free(void)
{
    store.file_unusable = 0;
    scalar_cache_clear();
    json_decref(store.file);
    store.file = NULL;
    json_decref(store.state);
    store.state = NULL;
    config_clear_conversation();
    json_decref(store.run);
    store.run = NULL;
    free_reported_presets();
    store.preset_warnings_emitted = 0;
}

/* Flat dotted keys take precedence over their nested spelling. */
static json_t *object_get_dotted(json_t *root, const char *key)
{
    if (!root)
        return NULL;

    json_t *value = json_object_get(root, key);
    if (value)
        return value;

    json_t *object = root;
    const char *segment = key;
    for (;;) {
        const char *dot = strchr(segment, '.');
        if (!dot)
            return json_object_get(object, segment);

        char segment_name[64];
        size_t segment_length = (size_t)(dot - segment);
        if (segment_length >= sizeof segment_name)
            return NULL;
        memcpy(segment_name, segment, segment_length);
        segment_name[segment_length] = '\0';

        object = json_object_get(object, segment_name);
        if (!json_is_object(object))
            return NULL;
        segment = dot + 1;
    }
}

static const char *object_get_string(json_t *root, const char *key)
{
    return scalar_as_string(object_get_dotted(root, key));
}

static void add_object_key(char ***keys, size_t *count, size_t *capacity, const char *key,
                           size_t key_length)
{
    for (size_t i = 0; i < *count; i++) {
        if (strlen((*keys)[i]) == key_length && strncmp((*keys)[i], key, key_length) == 0)
            return;
    }

    if (*count == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 8;
        *keys = xrealloc(*keys, *capacity * sizeof(**keys));
    }

    char *copy = xmalloc(key_length + 1);
    memcpy(copy, key, key_length);
    copy[key_length] = '\0';
    (*keys)[(*count)++] = copy;
}

/* Include both nested members and immediate children represented by flat dotted keys. */
static void collect_object_keys(json_t *tier, const char *key, char ***keys, size_t *count,
                                size_t *capacity)
{
    if (!json_is_object(tier))
        return;

    const char *member_name;
    json_t *member_value;
    json_t *object = object_get_dotted(tier, key);
    if (json_is_object(object)) {
        json_object_foreach(object, member_name, member_value)
            add_object_key(keys, count, capacity, member_name, strlen(member_name));
    }

    size_t prefix_length = strlen(key);
    json_object_foreach(tier, member_name, member_value)
    {
        if (strncmp(member_name, key, prefix_length) != 0 || member_name[prefix_length] != '.')
            continue;

        const char *child = member_name + prefix_length + 1;
        const char *dot = strchr(child, '.');
        size_t child_length = dot ? (size_t)(dot - child) : strlen(child);
        if (child_length)
            add_object_key(keys, count, capacity, child, child_length);
    }
}

static const char *resolve(const char *key, int skip_empty);

/* Saved selections may predate a provider-id rename; compare identities, not spellings. */
static int provider_id_equals(const char *left, const char *right)
{
    return strcmp(provider_canonical_id(left), provider_canonical_id(right)) == 0;
}

/* A model or effort stored beside a provider applies only while that provider is active. */
static int provider_binding_allows(json_t *tier, const char *key)
{
    if (strcmp(key, "model") != 0 && strcmp(key, "effort") != 0)
        return 1;

    const char *bound_provider = object_get_string(tier, "provider");
    if (!bound_provider || !*bound_provider)
        return 1;

    const char *active_provider = resolve("provider", 0);
    return active_provider && *active_provider &&
           provider_id_equals(active_provider, bound_provider);
}

static int value_present(const char *value, int skip_empty)
{
    return value && (!skip_empty || *value);
}

static const char *apply_default_sentinel(const char *value, const struct config_setting *setting)
{
    if (value && strcmp(value, CONFIG_VALUE_DEFAULT) == 0)
        return setting ? setting->default_value : NULL;
    return value;
}

/* A sentinel reports the tier that contains it even though its resolved value may be NULL. */
static const char *resolve_with_source(const char *key, int skip_empty, int skip_run,
                                       const char **source)
{
    const struct config_setting *setting = find_setting(key);
    const char *value = skip_run ? NULL : object_get_string(store.run, key);
    const char *value_source = "default";

    if (value_present(value, skip_empty)) {
        value_source = "run";
    } else {
        const char *conversation_value = object_get_string(store.conversation, key);
        const char *environment_value = setting ? getenv(setting->env_var) : NULL;
        const char *state_value = object_get_string(store.state, key);
        const char *file_value = object_get_string(store.file, key);

        if (value_present(conversation_value, skip_empty) &&
            provider_binding_allows(store.conversation, key)) {
            value = conversation_value;
            value_source = "conversation";
        } else if (value_present(environment_value, skip_empty)) {
            value = environment_value;
            value_source = "env";
        } else if (value_present(state_value, skip_empty) &&
                   provider_binding_allows(store.state, key)) {
            value = state_value;
            value_source = "state";
        } else if (value_present(file_value, skip_empty) &&
                   provider_binding_allows(store.file, key)) {
            value = file_value;
            value_source = "config";
        } else {
            value = setting ? setting->default_value : NULL;
        }
    }

    if (source)
        *source = value_source;
    return apply_default_sentinel(value, setting);
}

static const char *resolve(const char *key, int skip_empty)
{
    return resolve_with_source(key, skip_empty, 0, NULL);
}

static int setting_skips_empty(const struct config_setting *setting)
{
    return setting && !setting->keep_empty;
}

const char *config_str(const char *key)
{
    return resolve(key, setting_skips_empty(find_setting(key)));
}

const char *config_source(const char *key)
{
    const char *source;
    resolve_with_source(key, setting_skips_empty(find_setting(key)), 0, &source);
    return source;
}

const char *config_str_below_run(const char *key)
{
    return resolve_with_source(key, setting_skips_empty(find_setting(key)), 1, NULL);
}

const char *config_str_nonempty(const char *key)
{
    return resolve(key, 1);
}

const char *config_default(const char *key)
{
    const struct config_setting *setting = find_setting(key);
    return setting ? setting->default_value : NULL;
}

const json_t *config_json_node(const char *key)
{
    json_t *node = object_get_dotted(store.state, key);
    return node ? node : object_get_dotted(store.file, key);
}

size_t config_object_keys(const char *key, char ***out)
{
    char **keys = NULL;
    size_t count = 0;
    size_t capacity = 0;

    collect_object_keys(store.file, key, &keys, &count, &capacity);
    collect_object_keys(store.state, key, &keys, &count, &capacity);
    *out = keys;
    return count;
}

static int value_in_bounds(const struct config_setting *setting, long value)
{
    if (!setting)
        return 1;
    if (setting->min && value < setting->min)
        return 0;
    if (setting->max && value > setting->max)
        return 0;
    return 1;
}

static long parse_scaled(const char *str, long kilo)
{
    if (!str || !*str)
        return 0;

    char *end;
    errno = 0;
    long value = strtol(str, &end, 10);
    if (end == str || value <= 0 || errno == ERANGE)
        return 0;
    while (*end == ' ' || *end == '\t')
        end++;

    long multiplier = 1;
    switch (*end) {
    case 'k':
    case 'K':
        multiplier = kilo;
        end++;
        break;
    case 'm':
    case 'M':
        multiplier = kilo * kilo;
        end++;
        break;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0' || value > LONG_MAX / multiplier)
        return 0;
    return value * multiplier;
}

long parse_size(const char *str)
{
    return parse_scaled(str, 1024L);
}

long parse_token_count(const char *str)
{
    return parse_scaled(str, 1000L);
}

long parse_duration_ms(const char *str)
{
    if (!str || !*str)
        return -1;

    char *end;
    errno = 0;
    long value = strtol(str, &end, 10);
    if (end == str || value < 0 || errno == ERANGE)
        return -1;
    while (*end == ' ' || *end == '\t')
        end++;

    long multiplier;
    /* Match "ms" before "m". */
    if ((end[0] == 'm' || end[0] == 'M') && (end[1] == 's' || end[1] == 'S')) {
        multiplier = 1;
        end += 2;
    } else if (*end == '\0' || *end == 's' || *end == 'S') {
        multiplier = 1000;
        if (*end)
            end++;
    } else if (*end == 'm' || *end == 'M') {
        multiplier = 60000;
        end++;
    } else if (*end == 'h' || *end == 'H') {
        multiplier = 3600000;
        end++;
    } else {
        return -1;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0' || (multiplier > 1 && value > LONG_MAX / multiplier))
        return -1;
    return value * multiplier;
}

int config_int(const char *key)
{
    const struct config_setting *setting = find_setting(key);
    int value;

    if (parse_int(resolve(key, 1), &value) && value >= 0 && value_in_bounds(setting, value))
        return value;
    return setting && parse_int(setting->default_value, &value) ? value : 0;
}

/* Returns -1 rather than treating an unknown spelling as truthy. */
static int parse_bool(const char *value)
{
    if (!value || !*value)
        return -1;
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0)
        return 1;
    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0)
        return 0;
    return -1;
}

int config_bool(const char *key)
{
    int value = parse_bool(resolve(key, 1));
    if (value < 0) {
        const struct config_setting *setting = find_setting(key);
        value = setting ? parse_bool(setting->default_value) : -1;
    }
    return value > 0;
}

int config_bool_or(const char *key, int default_value)
{
    int value = parse_bool(resolve(key, 1));
    return value < 0 ? !!default_value : value;
}

const char *config_scoped_str(const char *prefix, const char *leaf)
{
    if (!prefix)
        return NULL;
    char *key = xasprintf("%s.%s", prefix, leaf);
    const char *value = config_str(key);
    free(key);
    return value;
}

int config_scoped_bool_or(const char *prefix, const char *leaf, int fallback)
{
    if (!prefix)
        return fallback;
    char *key = xasprintf("%s.%s", prefix, leaf);
    int value = config_bool_or(key, fallback);
    free(key);
    return value;
}

int config_scoped_int(const char *prefix, const char *leaf)
{
    if (!prefix)
        return 0;
    char *key = xasprintf("%s.%s", prefix, leaf);
    int value = config_int(key);
    free(key);
    return value;
}

/* A prompt file larger than this is almost certainly a mistake; refusing it beats silently
 * cutting instructions mid-sentence. */
#define PROMPT_FILE_CAP (64u * 1024u)

char *config_prompt_expand(const char *value, char **error)
{
    if (error)
        *error = NULL;
    if (value[0] != '@')
        return xstrdup(value);

    const char *spec = value + 1;
    char *path;
    if (spec[0] == '~')
        path = path_expand_home(spec);
    else if (spec[0] == '/')
        path = xstrdup(spec);
    else
        path = xdg_hax_config_path(spec);
    if (!path) {
        if (error)
            *error = xasprintf("couldn't resolve prompt file '%s'", spec);
        return NULL;
    }

    size_t len = 0;
    int truncated = 0;
    char *content = slurp_file_capped(path, PROMPT_FILE_CAP, &len, &truncated);
    if (!content) {
        if (error)
            *error = xasprintf("couldn't read prompt file %s", path);
        free(path);
        return NULL;
    }
    if (truncated) {
        if (error)
            *error = xasprintf("prompt file %s exceeds %u bytes", path, PROMPT_FILE_CAP);
        free(content);
        free(path);
        return NULL;
    }
    free(path);

    /* Provider JSON requires NUL-free, valid UTF-8. */
    char *clean = utf8_sanitize(content, len);
    free(content);
    size_t n = strlen(clean);
    while (n > 0 && (clean[n - 1] == '\n' || clean[n - 1] == '\r'))
        clean[--n] = '\0';
    return clean;
}

static int kind_value_valid(const struct config_setting *setting, const char *value)
{
    switch (setting->kind) {
    case CONFIG_KIND_INT: {
        int parsed;
        return parse_int(value, &parsed) && parsed >= 0 && value_in_bounds(setting, parsed);
    }
    case CONFIG_KIND_SIZE: {
        long parsed = parse_size(value);
        return parsed > 0 && value_in_bounds(setting, parsed);
    }
    case CONFIG_KIND_TOKENS: {
        long parsed = parse_token_count(value);
        return parsed > 0 && value_in_bounds(setting, parsed);
    }
    case CONFIG_KIND_DURATION: {
        long parsed = parse_duration_ms(value);
        return parsed >= 0 && value_in_bounds(setting, parsed);
    }
    case CONFIG_KIND_STRING:
        return 1;
    }
    return 0;
}

static const char *find_choice(const char *choices, const char *value, size_t *choice_length)
{
    size_t value_length = strlen(value);
    const char *choice = choices;

    for (;;) {
        const char *separator = strchr(choice, '|');
        size_t length = separator ? (size_t)(separator - choice) : strlen(choice);
        if (length == value_length && strncasecmp(choice, value, length) == 0) {
            if (choice_length)
                *choice_length = length;
            return choice;
        }
        if (!separator)
            return NULL;
        choice = separator + 1;
    }
}

static int choice_value_valid(const struct config_setting *setting, const char *value)
{
    /* Boolean aliases apply only to string settings, not numeric settings with symbolic choices. */
    if (setting->kind == CONFIG_KIND_STRING && strcmp(setting->choices, CONFIG_CHOICES_BOOL) == 0)
        return parse_bool(value) >= 0;
    if (setting->kind == CONFIG_KIND_STRING &&
        strcmp(setting->choices, CONFIG_CHOICES_TRISTATE) == 0)
        return strcasecmp(value, "auto") == 0 || parse_bool(value) >= 0;
    return find_choice(setting->choices, value, NULL) != NULL;
}

int config_value_valid(const struct config_setting *setting, const char *value)
{
    if (!setting || !value)
        return 0;
    if (setting->choices && choice_value_valid(setting, value))
        return 1;
    if (setting->choices && setting->kind == CONFIG_KIND_STRING)
        return 0;
    return kind_value_valid(setting, value);
}

static void kind_value_hint(const struct config_setting *setting, char *buffer, size_t size)
{
    switch (setting->kind) {
    case CONFIG_KIND_INT:
        if (setting->min && setting->max)
            snprintf(buffer, size, "a whole number from %ld to %ld", setting->min, setting->max);
        else if (setting->max)
            snprintf(buffer, size, "a whole number up to %ld", setting->max);
        else if (setting->min)
            snprintf(buffer, size, "a whole number of at least %ld", setting->min);
        else
            snprintf(buffer, size, "a whole number");
        break;
    case CONFIG_KIND_SIZE:
        snprintf(buffer, size, "a byte size like 64k or 1M (k = 1024)");
        break;
    case CONFIG_KIND_TOKENS:
        snprintf(buffer, size, "a token count like 200k or 1M (k = 1000)");
        break;
    case CONFIG_KIND_DURATION:
        snprintf(buffer, size, "a duration like 2s or 500ms");
        break;
    case CONFIG_KIND_STRING:
        buffer[0] = '\0';
        break;
    }
}

void config_value_hint(const struct config_setting *setting, char *buffer, size_t size)
{
    if (size == 0)
        return;
    buffer[0] = '\0';
    if (!setting)
        return;
    if (setting->kind == CONFIG_KIND_STRING) {
        if (setting->choices)
            snprintf(buffer, size, "%s", setting->choices);
        return;
    }

    char kind_hint[64];
    kind_value_hint(setting, kind_hint, sizeof(kind_hint));
    if (setting->choices && setting->example)
        snprintf(buffer, size, "%s, or %s; e.g. %s", setting->choices, kind_hint, setting->example);
    else if (setting->choices)
        snprintf(buffer, size, "%s, or %s", setting->choices, kind_hint);
    else if (setting->example)
        snprintf(buffer, size, "%s; e.g. %s", kind_hint, setting->example);
    else
        snprintf(buffer, size, "%s", kind_hint);
}

char *config_value_canonical(const struct config_setting *setting, const char *value)
{
    if (!setting || !value || !setting->choices ||
        (setting->kind == CONFIG_KIND_STRING && strcmp(setting->choices, CONFIG_CHOICES_BOOL) == 0))
        return NULL;

    size_t choice_length;
    const char *choice = find_choice(setting->choices, value, &choice_length);
    if (!choice)
        return NULL;

    char *canonical = xmalloc(choice_length + 1);
    memcpy(canonical, choice, choice_length);
    canonical[choice_length] = '\0';
    return canonical;
}

long config_size(const char *key)
{
    const struct config_setting *setting = find_setting(key);
    long value = parse_size(resolve(key, 1));
    if (value > 0 && value_in_bounds(setting, value))
        return value;
    return setting ? parse_size(setting->default_value) : 0;
}

long config_tokens(const char *key)
{
    const struct config_setting *setting = find_setting(key);
    long value = parse_token_count(resolve(key, 1));
    if (value > 0 && value_in_bounds(setting, value))
        return value;
    return setting ? parse_token_count(setting->default_value) : 0;
}

long config_duration_ms(const char *key)
{
    const struct config_setting *setting = find_setting(key);
    long value = parse_duration_ms(resolve(key, 1));
    if (value >= 0 && value_in_bounds(setting, value))
        return value;
    value = setting ? parse_duration_ms(setting->default_value) : -1;
    return value < 0 ? 0 : value;
}

static void set_tier_value(json_t **tier, const char *key, const char *value)
{
    if (!*tier)
        *tier = json_object();
    if (value)
        json_object_set_new(*tier, key, json_string(value));
    else
        json_object_del(*tier, key);
}

void config_set_override(const char *key, const char *value)
{
    set_tier_value(&store.run, key, value);
}

void config_set_conversation(const char *key, const char *value)
{
    set_tier_value(&store.conversation, key, value);
}

static void set_writable_tier(enum config_tier tier, const char *key, const char *value)
{
    if (tier == CONFIG_TIER_CONVERSATION)
        config_set_conversation(key, value);
    else
        config_set_override(key, value);
}

void config_clear_conversation(void)
{
    json_decref(store.conversation);
    store.conversation = NULL;
}

void config_preset_exit(enum config_tier tier)
{
    /* The name is shadowed with the empty sentinel rather than deleted, so a
     * lower-tier name can't resurface as a stance that isn't applied; the
     * prompt keys are dropped outright, since "" would mean "replace with
     * nothing" instead of "resolve one normally".
     *
     * The stance's tint needs no undoing: it was never written here, and
     * clearing the "tint" key would take an explicit /config tint down with
     * it. Dropping the name is enough — the display layer stops finding a
     * stance to read a tint off. */
    if (tier == CONFIG_TIER_RUN) {
        config_set_override("preset", "");
        config_set_override("system_prompt", NULL);
        config_set_override("system_prompt_append", NULL);
        /* A stance restored from the resumed conversation sits *below* those
         * overrides, where deleting the run value would expose it again. The
         * run has made its own selection, so that stance is over. */
        config_set_conversation("preset", NULL);
        config_set_conversation("system_prompt", NULL);
        config_set_conversation("system_prompt_append", NULL);
        return;
    }
    config_set_conversation("preset", "");
    config_set_conversation("system_prompt", NULL);
    config_set_conversation("system_prompt_append", NULL);
}

int config_restore_selection(enum config_tier tier, const char *provider, const char *model,
                             const char *effort, const char *preset, char **error)
{
    if (error)
        *error = NULL;
    if (provider && *provider && strcmp(provider, "none") != 0) {
        set_writable_tier(tier, "provider", provider);
        set_writable_tier(tier, "model", (model && *model) ? model : CONFIG_VALUE_DEFAULT);
        set_writable_tier(tier, "effort", (effort && *effort) ? effort : CONFIG_VALUE_DEFAULT);
    }

    config_preset_exit(tier);
    if (!preset || !*preset)
        return 0;
    if (config_preset_apply(preset, tier, error) != 0) {
        config_preset_exit(tier);
        return -1;
    }
    return 0;
}

struct config_snapshot {
    json_t *run;
    json_t *conversation;
};

struct config_snapshot *config_snapshot_take(void)
{
    struct config_snapshot *snapshot = xmalloc(sizeof(*snapshot));
    snapshot->run = store.run ? json_deep_copy(store.run) : NULL;
    snapshot->conversation = store.conversation ? json_deep_copy(store.conversation) : NULL;
    return snapshot;
}

void config_snapshot_restore(struct config_snapshot *snapshot)
{
    if (!snapshot)
        return;

    json_decref(store.run);
    store.run = snapshot->run;
    json_decref(store.conversation);
    store.conversation = snapshot->conversation;
    free(snapshot);
}

void config_snapshot_free(struct config_snapshot *snapshot)
{
    if (!snapshot)
        return;
    json_decref(snapshot->run);
    json_decref(snapshot->conversation);
    free(snapshot);
}

static void set_nested(json_t *root, const char *key, const char *value)
{
    json_t *object = root;
    const char *segment = key;

    for (;;) {
        const char *dot = strchr(segment, '.');
        if (!dot) {
            if (value)
                json_object_set_new(object, segment, json_string(value));
            else
                json_object_del(object, segment);
            return;
        }

        char segment_name[64];
        size_t segment_length = (size_t)(dot - segment);
        if (segment_length >= sizeof segment_name)
            return;
        memcpy(segment_name, segment, segment_length);
        segment_name[segment_length] = '\0';

        json_t *child = json_object_get(object, segment_name);
        if (!json_is_object(child)) {
            child = json_object();
            json_object_set_new(object, segment_name, child);
        }
        object = child;
        segment = dot + 1;
    }
}

static int write_json_atomic(const char *path, json_t *object)
{
    int result = -1;
    int fd = -1;
    FILE *file = NULL;
    char *temp_path = NULL;

    /* Renaming onto config.json would replace a symlink instead of updating its target. */
    char *destination = fs_resolve_link_target(path);
    if (!destination)
        return -1;

    char *directory = xstrdup(destination);
    fs_mkdir_p(dirname(directory));
    free(directory);

    temp_path = xasprintf("%s.tmp.XXXXXX", destination);
    fd = mkstemp(temp_path);
    if (fd < 0)
        goto out;

    /* fchmod preserves the 0600 contract even under a restrictive process umask. */
    if (fchmod(fd, 0600) != 0)
        goto out;

    file = fdopen(fd, "w");
    if (!file)
        goto out;
    fd = -1;

    if (json_dumpf(object, file, JSON_INDENT(2) | JSON_PRESERVE_ORDER) != 0)
        goto out;
    if (fclose(file) != 0) {
        file = NULL;
        goto out;
    }
    file = NULL;

    if (rename(temp_path, destination) != 0)
        goto out;
    result = 0;

out:
    if (file)
        fclose(file);
    if (fd >= 0)
        close(fd);
    if (result != 0 && temp_path)
        unlink(temp_path);
    free(temp_path);
    free(destination);
    return result;
}

/* `path` is consumed. Commit the in-memory copy only after the disk write succeeds. */
static int persist_tier(json_t **tier, char *path, const char *key, const char *value)
{
    if (!path)
        return -1;

    json_t *updated = *tier ? json_deep_copy(*tier) : json_object();
    if (!updated) {
        free(path);
        return -1;
    }

    /* A flat dotted key would otherwise shadow the nested value written below. */
    json_object_del(updated, key);
    set_nested(updated, key, value);

    int result = write_json_atomic(path, updated);
    free(path);
    if (result != 0) {
        json_decref(updated);
        return -1;
    }

    scalar_cache_clear();
    json_decref(*tier);
    *tier = updated;
    return 0;
}

int config_persist(const char *key, const char *value)
{
    if (store.file_unusable)
        return -1;
    return persist_tier(&store.file, xdg_hax_config_path("config.json"), key, value);
}

int config_persist_state(const char *key, const char *value)
{
    return persist_tier(&store.state, xdg_hax_state_path("state.json"), key, value);
}

int config_persist_selection(const char *provider, const char *model, const char *effort)
{
    if (!provider || !*provider)
        return -1;
    char *path = xdg_hax_state_path("state.json");
    if (!path)
        return -1;
    json_t *updated = store.state ? json_deep_copy(store.state) : json_object();
    if (!updated) {
        free(path);
        return -1;
    }

    const char *previous_provider = object_get_string(store.state, "provider");
    int provider_changed = !previous_provider || !provider_id_equals(previous_provider, provider);

    /* A selection replaces the preset stance that would otherwise reapply on launch. */
    json_object_del(updated, "preset");
    json_object_set_new(updated, "provider", json_string(provider));
    if (model || provider_changed)
        json_object_set_new(updated, "model", json_string(model ? model : CONFIG_VALUE_DEFAULT));
    if (effort || provider_changed)
        json_object_set_new(updated, "effort", json_string(effort ? effort : CONFIG_VALUE_DEFAULT));

    int result = write_json_atomic(path, updated);
    free(path);
    if (result != 0) {
        json_decref(updated);
        return -1;
    }

    scalar_cache_clear();
    json_decref(store.state);
    store.state = updated;
    return 0;
}

/* Preset names are literal object members, so dots in a name do not become path separators. */
static const json_t *preset_node(const char *name)
{
    json_t *const tiers[] = {store.state, store.file};
    for (size_t i = 0; i < sizeof(tiers) / sizeof(*tiers); i++) {
        json_t *presets = object_get_dotted(tiers[i], "presets");
        json_t *preset = json_is_object(presets) ? json_object_get(presets, name) : NULL;
        if (json_is_object(preset))
            return preset;
    }

    /* Preserve the one-level flat form: {"presets.<name>": {...}}. */
    char *key = xasprintf("presets.%s", name);
    const json_t *preset = config_json_node(key);
    free(key);
    return json_is_object(preset) ? preset : NULL;
}

/* Construction- and startup-bound settings cannot be honored by a mid-session preset. Tint is
 * read from the active preset instead of copied into a tier because /config can override it. */
static const char *const PRESET_SETTING_KEYS[] = {
    "provider", "model", "effort", "system_prompt", "system_prompt_append", "tint",
};

static int preset_key_allowed(const char *key)
{
    if (strcmp(key, "description") == 0)
        return 1;
    for (size_t i = 0; i < sizeof(PRESET_SETTING_KEYS) / sizeof(*PRESET_SETTING_KEYS); i++) {
        if (strcmp(key, PRESET_SETTING_KEYS[i]) == 0)
            return 1;
    }
    return 0;
}

static const char *preset_member_string(const json_t *preset, const char *member)
{
    return scalar_as_string(json_object_get((json_t *)preset, member));
}

/* Apply and enumeration share validation so every advertised preset is appliable. */
static int preset_validate(const json_t *preset, const char *name, char **error)
{
    const char *member_name;
    json_t *member;

    json_object_foreach((json_t *)preset, member_name, member)
    {
        if (!preset_key_allowed(member_name)) {
            if (error)
                *error = xasprintf(
                    "preset '%s': '%s' is not presettable (allowed: provider, model, effort, "
                    "system_prompt, system_prompt_append, tint); endpoint settings belong in a "
                    "providers.<name> block, context/recording in the --bare/--no-session flags",
                    name, member_name);
            return -1;
        }
        if (!scalar_as_string(member)) {
            if (error)
                *error = xasprintf("preset '%s': '%s' must be a scalar", name, member_name);
            return -1;
        }
    }

    const char *provider = preset_member_string(preset, "provider");
    if (!provider || !*provider) {
        if (error)
            *error = xasprintf("preset '%s' must name a provider", name);
        return -1;
    }

    const struct config_setting *tint_setting = find_setting("tint");
    const char *tint = preset_member_string(preset, "tint");
    if (tint && !config_value_valid(tint_setting, tint)) {
        if (error)
            *error = xasprintf("preset '%s': unknown tint '%s' (expected %s)", name, tint,
                               tint_setting ? tint_setting->choices : "");
        return -1;
    }

    /* @file prompts are probed here so an advertised preset cannot fail on apply. */
    static const char *const prompt_keys[] = {"system_prompt", "system_prompt_append"};
    for (size_t i = 0; i < sizeof(prompt_keys) / sizeof(*prompt_keys); i++) {
        const char *value = preset_member_string(preset, prompt_keys[i]);
        if (!value || value[0] != '@')
            continue;
        char *expand_error = NULL;
        char *text = config_prompt_expand(value, &expand_error);
        if (!text) {
            if (error)
                *error = xasprintf("preset '%s': %s", name,
                                   expand_error ? expand_error : "unreadable prompt file");
            free(expand_error);
            return -1;
        }
        free(text);
    }
    return 0;
}

static int preset_defect_reported(const char *name)
{
    for (size_t i = 0; i < store.n_reported_presets; i++) {
        if (strcmp(store.reported_presets[i], name) == 0)
            return 1;
    }
    return 0;
}

/* A failed apply returns the defect to its caller, and every caller surfaces it; recording
 * the name here keeps the enumeration warning from repeating the same message. */
static void report_preset_defect(const char *name)
{
    if (preset_defect_reported(name))
        return;
    store.reported_presets = xrealloc(store.reported_presets, (store.n_reported_presets + 1) *
                                                                  sizeof(*store.reported_presets));
    store.reported_presets[store.n_reported_presets++] = xstrdup(name);
}

static void free_reported_presets(void)
{
    for (size_t i = 0; i < store.n_reported_presets; i++)
        free(store.reported_presets[i]);
    free(store.reported_presets);
    store.reported_presets = NULL;
    store.n_reported_presets = 0;
}

int config_preset_apply(const char *name, enum config_tier tier, char **error)
{
    if (error)
        *error = NULL;

    const json_t *preset = preset_node(name);
    if (!preset) {
        if (error)
            *error = xasprintf("unknown preset '%s' (define a presets.%s block in config.json)",
                               name, name);
        report_preset_defect(name);
        return -1;
    }
    if (preset_validate(preset, name, error) != 0) {
        report_preset_defect(name);
        return -1;
    }

    const char *model = preset_member_string(preset, "model");
    const char *effort = preset_member_string(preset, "effort");

    /* End the previous stance first so an omitted system prompt cannot inherit from it. */
    config_preset_exit(tier);
    set_writable_tier(tier, "provider", preset_member_string(preset, "provider"));
    set_writable_tier(tier, "model", model ? model : CONFIG_VALUE_DEFAULT);
    set_writable_tier(tier, "effort", effort ? effort : CONFIG_VALUE_DEFAULT);
    set_writable_tier(tier, "system_prompt", preset_member_string(preset, "system_prompt"));
    set_writable_tier(tier, "system_prompt_append",
                      preset_member_string(preset, "system_prompt_append"));
    set_writable_tier(tier, "tint", NULL);
    set_writable_tier(tier, "preset", name);
    return 0;
}

static const char *preset_value(const char *name, const char *member)
{
    const json_t *preset = preset_node(name);
    return preset ? preset_member_string(preset, member) : NULL;
}

const char *config_preset_description(const char *name)
{
    return preset_value(name, "description");
}

const char *config_preset_tint(const char *name)
{
    return preset_value(name, "tint");
}

const char *config_preset_provider(const char *name)
{
    return preset_value(name, "provider");
}

const char *config_preset_model(const char *name)
{
    return preset_value(name, "model");
}

const char *config_preset_effort(const char *name)
{
    return preset_value(name, "effort");
}

static json_t *preset_nested_member(json_t *tier, const char *name)
{
    json_t *presets = object_get_dotted(tier, "presets");
    return json_is_object(presets) ? json_object_get(presets, name) : NULL;
}

static json_t *preset_member(json_t *tier, const char *name)
{
    json_t *preset = preset_nested_member(tier, name);
    if (preset)
        return preset;

    char *key = xasprintf("presets.%s", name);
    preset = object_get_dotted(tier, key);
    free(key);
    return preset;
}

/* A nested state definition would outrank the nested config definition written by save. */
static int state_defines_preset(const char *name)
{
    return json_is_object(preset_nested_member(store.state, name));
}

int config_preset_exists(const char *name)
{
    if (!name || !*name)
        return 0;
    /* Any type counts, not just an appliable object: a half-written
     * "work": "draft" is still the user's content under that name, and a save
     * that reported it absent would replace it without asking. */
    return preset_member(store.state, name) != NULL || preset_member(store.file, name) != NULL;
}

int config_preset_name_valid(const char *name)
{
    if (!name || !isalnum((unsigned char)name[0]))
        return 0;
    size_t n = strlen(name);
    if (n >= 64)
        return 0;
    for (size_t i = 1; i < n; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_')
            return 0;
    }
    return 1;
}

int config_preset_save(const char *name, const struct config_preset *definition, char **error)
{
    int result = -1;
    char *path = NULL;
    json_t *updated = NULL;
    json_t *preset = NULL;

    if (error)
        *error = NULL;
    if (!config_preset_name_valid(name)) {
        if (error)
            *error = xasprintf("'%s' can't be a preset name — use letters, digits, '.', '-' or "
                               "'_', starting with a letter or digit",
                               name ? name : "");
        goto out;
    }
    if (state_defines_preset(name)) {
        if (error)
            *error = xasprintf("preset '%s' is defined in state.json, which outranks the config "
                               "file — remove it there first",
                               name);
        goto out;
    }

    preset = json_object();
    if (!preset)
        goto out;

    /* Keep the user-facing identity fields first in the serialized object. */
    const struct {
        const char *key;
        const char *value;
    } members[] = {
        {"description", definition->description},
        {"tint", definition->tint},
        {"provider", definition->provider},
        {"model", definition->model},
        {"effort", definition->effort},
        {"system_prompt", definition->system_prompt},
        {"system_prompt_append", definition->system_prompt_append},
    };
    for (size_t i = 0; i < sizeof(members) / sizeof(*members); i++) {
        if (members[i].value)
            json_object_set_new(preset, members[i].key, json_string(members[i].value));
    }
    if (preset_validate(preset, name, error) != 0)
        goto out;

    path = xdg_hax_config_path("config.json");
    if (!path) {
        if (error)
            *error = xstrdup("couldn't locate the config directory");
        goto out;
    }
    if (store.file_unusable) {
        if (error)
            *error = xasprintf("couldn't read %s — fix or remove it first", path);
        goto out;
    }

    updated = store.file ? json_deep_copy(store.file) : json_object();
    if (!updated)
        goto out;

    /* Remove the flat fallback so the file does not retain two definitions of the same preset. */
    char *flat_key = xasprintf("presets.%s", name);
    json_object_del(updated, flat_key);
    free(flat_key);

    json_t *presets = json_object_get(updated, "presets");
    if (presets && !json_is_object(presets)) {
        if (error)
            *error = xasprintf("\"presets\" in %s is not a block of presets — fix it first", path);
        goto out;
    }
    if (!presets) {
        presets = json_object();
        if (!presets || json_object_set_new(updated, "presets", presets) != 0)
            goto out;
    }
    if (json_object_set(presets, name, preset) != 0)
        goto out;

    if (write_json_atomic(path, updated) != 0) {
        if (error)
            *error = xasprintf("couldn't write %s", path);
        goto out;
    }

    scalar_cache_clear();
    json_decref(store.file);
    store.file = updated;
    updated = NULL;
    result = 0;

out:
    if (result != 0 && error && !*error)
        *error = xstrdup("couldn't save preset");
    free(path);
    json_decref(updated);
    json_decref(preset);
    return result;
}

size_t config_preset_names(char ***out)
{
    char **names = NULL;
    size_t count = config_object_keys("presets", &names);
    size_t valid_count = 0;

    for (size_t i = 0; i < count; i++) {
        const json_t *preset = preset_node(names[i]);
        char *error = NULL;
        /* This runs on every prompt rebuild, so invalid definitions warn only once. */
        int quiet = store.preset_warnings_emitted || preset_defect_reported(names[i]);
        if (preset && preset_validate(preset, names[i], quiet ? NULL : &error) == 0) {
            names[valid_count++] = names[i];
            continue;
        }

        if (!quiet) {
            if (error)
                hax_warn("%s — ignoring it", error);
            else if (!preset)
                hax_warn("preset '%s' is not an object (define a presets.%s block) — "
                         "ignoring it",
                         names[i], names[i]);
        }
        free(error);
        free(names[i]);
    }

    store.preset_warnings_emitted = 1;
    *out = names;
    return valid_count;
}
