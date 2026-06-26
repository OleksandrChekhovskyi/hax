/* SPDX-License-Identifier: MIT */
#include "openrouter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "openai.h"
#include "probe.h"
#include "util.h"

/* Single-model lookup is small (one entry's worth of metadata, a few KB)
 * vs the full catalog (~430 KB at the time of writing, hundreds of
 * entries) — even on a slow link this rarely takes more than a second.
 * Failure is silent: we just leave the percentage display hidden,
 * which the user can also fill manually via HAX_CONTEXT_LIMIT. */
#define PROBE_TIMEOUT_S 5

/* Walk the per-model response shape:
 *     { "data": { "id": "...", "endpoints": [ { "context_length": N, ... }, ... ] } }
 *
 * One model can be served by several upstream providers (each is one
 * `endpoints[]` entry) with potentially different context windows.
 * Take the maximum — that matches the model-level `context_length`
 * the catalog and the OpenRouter UI advertise, which is what users
 * expect to see represented in the percentage display. An empty
 * `endpoints[]` (deprecated or restricted model) returns 0, which the
 * helper drops silently. `user` is unused — the URL already targets
 * one model, so there's no slug to match against here. */
static long extract_openrouter_context(const char *body, void *user)
{
    (void)user;
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return 0;
    long out = 0;
    json_t *data = json_object_get(root, "data");
    json_t *endpoints = data ? json_object_get(data, "endpoints") : NULL;
    if (json_is_array(endpoints)) {
        size_t i;
        json_t *entry;
        json_array_foreach(endpoints, i, entry)
        {
            json_t *ctx = json_object_get(entry, "context_length");
            if (json_is_integer(ctx) && json_integer_value(ctx) > out)
                out = (long)json_integer_value(ctx);
        }
    }
    json_decref(root);
    return out;
}

static void spawn_context_probe(struct provider *p, const char *api_key)
{
    /* context_limit is the user override — when it's usable, the agent
     * already prefers it, so there's nothing for the probe to add and we
     * save a network round-trip. Ask config_size — the same question the
     * display path asks — so an unparseable value falls back to
     * auto-detection instead of silently hiding the % display. */
    if (config_size("context_limit") > 0)
        return;
    const char *model = config_str("model");
    if (!model || !*model)
        return; /* nothing to look up — agent will surface the missing-model error */

    struct probe_args *a = xcalloc(1, sizeof(*a));
    /* OpenRouter model ids contain `/` (and sometimes `:variant`)
     * which are valid URL path-segment characters per RFC 3986
     * sub-delims, so concatenation works without percent-encoding —
     * confirmed against ids like `meta-llama/llama-3.2-3b-instruct:free`.
     * If a more exotic id ever needs escaping, this is the place to
     * add it. */
    a->url = xasprintf("https://openrouter.ai/api/v1/models/%s/endpoints", model);
    if (api_key && *api_key) {
        a->headers = xcalloc(2, sizeof(*a->headers));
        a->headers[0] = xasprintf("Authorization: Bearer %s", api_key);
        a->headers[1] = NULL;
    }
    a->timeout_s = PROBE_TIMEOUT_S;
    a->extract = extract_openrouter_context;
    a->target = &p->context_limit;
    /* Hand off to the openai destroy() — it owns the join, so we don't
     * need to track the handle locally. */
    openai_attach_probe(p, probe_context_limit_spawn(a));
}

/* Re-resolve the API key exactly as the constructor does (HAX_OPENAI_API_KEY,
 * then the OPENROUTER_API_KEY fallback). Borrowed. */
static const char *openrouter_api_key(void)
{
    const char *key = config_str("openai.api_key");
    if (!key || !*key)
        key = getenv("OPENROUTER_API_KEY");
    return (key && *key) ? key : NULL;
}

/* /model switched the model: the per-model /endpoints catalog gives a
 * different window, so re-probe. openai_context_probe_reset settles the prior
 * probe and clears the limit; spawn_context_probe re-reads config_str("model")
 * (the agent committed the new value before apply), so the explicit `model`
 * isn't needed here. */
static void openrouter_refresh_context(struct provider *p, const char *model)
{
    (void)model;
    openai_context_probe_reset(p);
    spawn_context_probe(p, openrouter_api_key());
}

struct provider *openrouter_provider_new(void)
{
    /* Fixed to openrouter.ai. HAX_OPENAI_BASE_URL is ignored (lock_base_url),
     * not rejected, so the OPENROUTER_API_KEY fallback and the public X-Title
     * attribution header can never reach an unrelated host, and a base URL
     * left set for another backend doesn't block selecting openrouter. Custom
     * endpoints belong on HAX_PROVIDER=openai-compatible. */
    const char *key = openrouter_api_key();

    const char *title = config_str("openrouter.title");
    if (!title || !*title)
        title = "hax";
    const char *referer = config_str("openrouter.referer");

    /* OpenRouter uses these for attribution on its public leaderboards and
     * usage dashboards. Both are optional from OpenRouter's side; we always
     * send X-Title (so traffic is recognizable) but only send HTTP-Referer
     * when the user explicitly opts in. */
    char *title_hdr = xasprintf("X-Title: %s", title);
    char *referer_hdr = (referer && *referer) ? xasprintf("HTTP-Referer: %s", referer) : NULL;

    const char *headers[3];
    size_t i = 0;
    headers[i++] = title_hdr;
    if (referer_hdr)
        headers[i++] = referer_hdr;
    headers[i] = NULL;

    /* HAX_SHOW_REASONING also acts as a request-side opt-in here:
     * many OpenRouter models gate CoT behind `reasoning.enabled=true`,
     * so without this flag the deltas the user asked to see never
     * arrive. When off, we fall back to the flat dialect so a plain
     * `reasoning_effort` still reaches models that honor it. The same
     * env var still gates the *display* side in the agent — provider
     * opt-in alone doesn't render anything. */
    int request_reasoning = config_bool("show_reasoning");

    struct openai_preset preset = {
        .display_name = "openrouter",
        .default_base_url = "https://openrouter.ai/api/v1",
        .api_key_env = "OPENROUTER_API_KEY",
        .send_cache_key_default = 1,
        .lock_base_url = 1,
        .extra_headers = headers,
        .reasoning_format = request_reasoning ? REASONING_NESTED : REASONING_FLAT,
        /* OpenRouter normalizes the full OpenAI-style ladder across upstreams
         * and maps an unsupported level to the nearest one, so the whole
         * ladder is safe to offer. */
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    /* Constructor copies headers internally, so the local strings can be
     * freed once it returns. */
    struct provider *p = openai_provider_new_preset(&preset);
    free(title_hdr);
    free(referer_hdr);
    if (p) {
        p->refresh_context = openrouter_refresh_context;
        spawn_context_probe(p, key);
    }
    return p;
}

/* Usable iff a key is configured — HAX_OPENAI_API_KEY or the OPENROUTER_API_KEY
 * fallback the preset already consults. */
static int openrouter_available(const char **reason)
{
    return openai_key_available("OPENROUTER_API_KEY", reason);
}

const struct provider_factory PROVIDER_OPENROUTER = {
    .name = "openrouter",
    .new = openrouter_provider_new,
    .available = openrouter_available,
};
