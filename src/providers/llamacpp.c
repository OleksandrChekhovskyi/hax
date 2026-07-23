/* SPDX-License-Identifier: MIT */
#include "llamacpp.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>

#include "config.h"
#include "openai.h"
#include "probe.h"
#include "util.h"
#include "transport/http.h"

/* Model probe runs synchronously at startup because HAX_MODEL must be
 * resolved before the first chat request body is built. The context-limit
 * probe is async (see spawn_context_probe) so a slow /props doesn't delay
 * the first prompt. Both stay short so a missing or slow server fails
 * cleanly. */
#define MODEL_PROBE_TIMEOUT_S 2
#define CTX_PROBE_TIMEOUT_S   5

/* Build a sibling URL with the same scheme/host/port as `base` but a
 * different path. Used to reach llama-server's `/props` (rooted, not under
 * `/v1`). Returns a heap-owned string or NULL on failure. */
static char *swap_path(const char *base, const char *new_path)
{
    CURLU *u = curl_url();
    if (!u)
        return NULL;
    char *out = NULL;
    if (curl_url_set(u, CURLUPART_URL, base, 0) == CURLUE_OK &&
        curl_url_set(u, CURLUPART_PATH, new_path, 0) == CURLUE_OK) {
        char *built = NULL;
        if (curl_url_get(u, CURLUPART_URL, &built, 0) == CURLUE_OK) {
            out = xstrdup(built);
            curl_free(built);
        }
    }
    curl_url_cleanup(u);
    return out;
}

/* Resolve the base URL the openai constructor will actually use: an explicit
 * HAX_OPENAI_BASE_URL if set, else the llamacpp.port default. Trailing slash
 * normalized so the probe and stream() paths match. Caller frees. */
static char *resolve_base_url(void)
{
    char *default_url = xasprintf("http://127.0.0.1:%d/v1", config_int("llamacpp.port"));
    const char *base_env = config_str("openai.base_url");
    char *resolved = dup_trim_trailing_slash((base_env && *base_env) ? base_env : default_url);
    free(default_url);
    return resolved;
}

/* Build the /props probe URL, scoped to `model` with a `?model=` query when
 * one is given. Router-mode llama-server loads several models on demand, so
 * /props must name which one; the param is ignored (harmless) in the common
 * single-model case. `model` (which may contain '/' or spaces) is URL-
 * encoded. Caller frees. */
char *llamacpp_props_url(const char *base_url, const char *model)
{
    char *url = swap_path(base_url, "/props");
    if (!url || !model || !*model)
        return url;

    CURLU *u = curl_url();
    if (!u)
        return url;
    char *query = xasprintf("model=%s", model);
    char *out = url;
    if (curl_url_set(u, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_set(u, CURLUPART_QUERY, query, CURLU_APPENDQUERY | CURLU_URLENCODE) == CURLUE_OK) {
        char *built = NULL;
        if (curl_url_get(u, CURLUPART_URL, &built, 0) == CURLUE_OK) {
            out = xstrdup(built);
            curl_free(built);
            free(url);
        }
    }
    free(query);
    curl_url_cleanup(u);
    return out;
}

/* The pure reconcile decision behind probe_model, split out for tests (no
 * HTTP): parse a /v1/models body and decide against `cur`. Returns 0 when
 * the list names at least one model — a resolvable state — with *adopt set
 * to a malloc'd replacement (the first served entry) when `cur` is unset or
 * absent from the list, or NULL when the configured model is served and
 * kept. Returns -1 (adopt untouched) for an unusable body or empty list. */
int llamacpp_reconcile_model(const char *body, const char *cur, char **adopt)
{
    *adopt = NULL;
    json_t *root = json_loads(body, 0, NULL);
    json_t *data = root ? json_object_get(root, "data") : NULL;
    const char *first = NULL;
    int present = 0;
    if (json_is_array(data)) {
        size_t n = json_array_size(data);
        for (size_t i = 0; i < n; i++) {
            json_t *id = json_object_get(json_array_get(data, i), "id");
            if (!json_is_string(id))
                continue;
            const char *s = json_string_value(id);
            if (!first)
                first = s;
            if (cur && *cur && strcmp(s, cur) == 0)
                present = 1;
        }
    }
    int rc = first ? 0 : -1;
    /* Unset or stale (not in the live list) → adopt what's served; a
     * still-valid configured model is left untouched. */
    if (first && (!cur || !*cur || !present))
        *adopt = xstrdup(first);
    json_decref(root);
    return rc;
}

char *llamacpp_model_warning(const char *configured, const char *served)
{
    char *configured_label = llamacpp_model_label(NULL, configured);
    char *served_label = llamacpp_model_label(NULL, served);
    char *warning;
    if (strcmp(configured_label, served_label) == 0)
        warning = xasprintf("llama.cpp: configured model is not served — using '%s'", served_label);
    else
        warning = xasprintf("llama.cpp: model '%s' is not served — using '%s'", configured_label,
                            served_label);
    free(served_label);
    free(configured_label);
    return warning;
}

/* Resolve the model to send, treating llama-server's catalog as live server
 * state rather than a user preference. The served model depends on how the
 * server was started — a single model, or (router mode) several loaded on
 * demand — and a value stored in config/state.json can be stale: the server
 * may have been restarted with a different model since. So reconcile against
 * the live /v1/models: keep the configured model only if the server is
 * actually serving it; otherwise adopt what it serves (the first entry — the
 * sole model in single-model mode, or the first router model). The effective
 * value is set as a session override (config tier), so it wins for this run
 * without rewriting a persisted choice that may become valid again.
 *
 * Returns 0 when a model could be resolved (reconciled, or an explicit one
 * trusted while the server is briefly unreachable). Returns -1 only when the
 * server is unreachable AND nothing is configured — a strong "is it running?"
 * signal the caller surfaces instead of a downstream "HAX_MODEL is required". */
static int probe_model(const char *base_url, const char *api_key)
{
    char *url = xasprintf("%s/models", base_url);
    char *auth = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *headers[] = {auth, NULL};
    char *body = NULL;
    int reached = http_get(url, auth ? headers : NULL, MODEL_PROBE_TIMEOUT_S, 0, NULL, NULL, &body,
                           NULL) == 0;

    const char *cur = config_str("model");
    int rc = -1;
    if (reached) {
        char *adopt = NULL;
        if (llamacpp_reconcile_model(body, cur, &adopt) == 0) {
            if (adopt) {
                /* Replacing an explicit value is announced; unset means
                 * ordinary auto-discovery and stays silent. */
                if (cur && *cur) {
                    char *warning = llamacpp_model_warning(cur, adopt);
                    hax_warn("%s", warning);
                    free(warning);
                }
                config_set_override("model", adopt);
                free(adopt);
            }
            rc = 0;
        }
    } else if (cur && *cur) {
        /* Unreachable but a model is explicitly configured: trust it and let
         * the first stream surface the real connection error, rather than
         * failing construction (which would drop the user out of the REPL). */
        rc = 0;
    }

    free(body);
    free(auth);
    free(url);
    return rc;
}

/* /props returns the live n_ctx the server was started with — the only
 * cross-version-stable way to learn the context window from llama-server.
 * `user` is unused (single global value, no key to match). */
static long extract_llamacpp_n_ctx(const char *body, void *user)
{
    (void)user;
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return 0;
    long out = 0;
    json_t *settings = json_object_get(root, "default_generation_settings");
    json_t *n_ctx = settings ? json_object_get(settings, "n_ctx") : NULL;
    if (json_is_integer(n_ctx) && json_integer_value(n_ctx) > 0)
        out = (long)json_integer_value(n_ctx);
    json_decref(root);
    return out;
}

/* /props also reports the loaded model's modalities ({"vision": bool,
 * ...}) — authoritative for image input, since vision requires an mmproj
 * projector loaded on this server instance, which no external catalog
 * can know. Absent on older servers → 0 (unknown, dropped). */
static long extract_llamacpp_image_input(const char *body, void *user)
{
    (void)user;
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return 0;
    long out = PROVIDER_IMG_UNKNOWN;
    json_t *modalities = json_object_get(root, "modalities");
    json_t *vision = modalities ? json_object_get(modalities, "vision") : NULL;
    if (json_is_boolean(vision))
        out = json_is_true(vision) ? PROVIDER_IMG_YES : PROVIDER_IMG_NO;
    json_decref(root);
    return out;
}

char *llamacpp_model_label(struct provider *p, const char *model)
{
    (void)p;
    size_t len = strlen(model);
    if (len <= 5 || strcasecmp(model + len - 5, ".gguf") != 0)
        return xstrdup(model);

    const char *base = strrchr(model, '/');
    const char *backslash = strrchr(model, '\\');
    if (!base || (backslash && backslash > base))
        base = backslash;
    base = base ? base + 1 : model;

    size_t stem_len = (size_t)(model + len - 5 - base);
    if (stem_len == 0)
        return xstrdup(model);
    char *label = xmalloc(stem_len + 1);
    memcpy(label, base, stem_len);
    label[stem_len] = '\0';
    return label;
}

static void spawn_context_probe(struct provider *p, const char *base_url, const char *api_key)
{
    char *url = llamacpp_props_url(base_url, config_str("model"));
    if (!url)
        return;

    struct probe_args *a = xcalloc(1, sizeof(*a));
    a->url = url;
    if (api_key && *api_key) {
        a->headers = xcalloc(2, sizeof(*a->headers));
        a->headers[0] = xasprintf("Authorization: Bearer %s", api_key);
        a->headers[1] = NULL;
    }
    a->timeout_s = CTX_PROBE_TIMEOUT_S;
    /* A usable user-supplied context_limit wins; skip that extraction so
     * the probe never overwrites the override the constructor stored. Ask
     * config_size — the same question the display path asks — so an
     * unparseable value falls back to auto-detection. The probe itself
     * still runs: /props is also the modalities source. */
    a->fields[0].extract = config_size("context_limit") > 0 ? NULL : extract_llamacpp_n_ctx;
    a->fields[0].target = &p->context_limit;
    a->fields[1].extract = extract_llamacpp_image_input;
    a->fields[1].target = &p->image_input;
    openai_attach_probe(p, probe_spawn(a));
}

/* A runtime /model switch (router mode serves several models) changes both
 * the context window and the vision capability, so re-probe /props for the
 * newly selected model. openai_context_probe_reset settles the prior probe
 * and clears context_limit + image_input to unknown; the agent commits the
 * new model to config before calling this, so props_url picks it up. */
static void llamacpp_refresh_context(struct provider *p, const char *model)
{
    (void)model;
    openai_context_probe_reset(p);
    char *resolved = resolve_base_url();
    const char *key = config_str("openai.api_key");
    spawn_context_probe(p, resolved, (key && *key) ? key : NULL);
    free(resolved);
}

struct provider *llamacpp_provider_new(const char *name)
{
    (void)name;
    /* The "8080" default lives in the config registry, so it's defined in
     * one place. */
    char *default_url = xasprintf("http://127.0.0.1:%d/v1", config_int("llamacpp.port"));

    /* Probe whichever URL the openai constructor will actually use, so a
     * user-supplied HAX_OPENAI_BASE_URL still benefits from auto-discovery.
     * Normalize the trailing slash up-front so the probe and the eventual
     * stream() target produce identical paths. */
    const char *base_env = config_str("openai.base_url");
    char *resolved = dup_trim_trailing_slash((base_env && *base_env) ? base_env : default_url);
    /* llama-server can be started with --api-key, in which case HAX_OPENAI_API_KEY
     * carries the matching token. Forward it to the probes too — otherwise an
     * authenticated server returns 401 on /v1/models and provider construction
     * fails even though the eventual chat request would have been authorized. */
    const char *key = config_str("openai.api_key");
    if (key && !*key)
        key = NULL;
    if (probe_model(resolved, key) != 0) {
        hax_err("llama.cpp: failed to auto-discover model from %s/models\n"
                "hax: is llama-server running? "
                "(set HAX_MODEL to skip probing, or adjust HAX_LLAMACPP_PORT / "
                "HAX_OPENAI_BASE_URL)",
                resolved);
        free(resolved);
        free(default_url);
        return NULL;
    }

    struct openai_preset preset = {
        .display_name = "llama.cpp",
        .default_base_url = default_url,
        .send_cache_key_default = 0,
        /* llama-server attaches `prompt_progress` to each prefill chunk
         * when this is set — the only provider we target today that
         * exposes server-side progress. */
        .emit_progress = 1,
        /* Qwen3 and other interleaved-thinking models served by
         * llama-server degrade and leak tool calls into the reasoning
         * channel unless their prior reasoning is fed back. Round-trip it
         * as reasoning_content (the field llama-server ingests). Disable
         * with HAX_REASONING_ROUNDTRIP=off. */
        .roundtrip_reasoning_field = "reasoning_content",
        /* If the server's context (-c / --ctx-size) is full, the reply
         * truncates to "length"; unlike ollama there's no per-request knob,
         * so the fix is a larger -c at launch. */
        .length_hint = "llama-server's context is full — restart it with a larger "
                       "-c / --ctx-size",
    };
    struct provider *p = openai_provider_new_preset(&preset);
    if (p) {
        p->model_label = llamacpp_model_label;
        p->refresh_context = llamacpp_refresh_context;
        /* Context-limit probe runs in the background: an older llama-server
         * without /props, or a proxy that doesn't expose it, just means the
         * percentage display is hidden — not a reason to refuse to start
         * (or to delay the first prompt). */
        spawn_context_probe(p, resolved, key);
    }
    free(resolved);
    free(default_url);
    return p;
}

/* Availability is "is llama-server up": a bounded GET on the same /models
 * the model probe uses. Resolves the base URL exactly as the constructor
 * does so the probe targets the server the user would actually reach. */
static int llamacpp_available(const char *name, const char **reason)
{
    (void)name;
    char *resolved = resolve_base_url();
    const char *key = config_str("openai.api_key");
    int ok = openai_base_url_reachable(resolved, (key && *key) ? key : NULL, reason);
    free(resolved);
    return ok;
}

const struct provider_factory PROVIDER_LLAMACPP = {
    .name = "llama.cpp",
    .new = llamacpp_provider_new,
    .available = llamacpp_available,
};
