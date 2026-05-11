/* SPDX-License-Identifier: MIT */
#include "openrouter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    /* HAX_CONTEXT_LIMIT is the user override — when it's set, the agent
     * already prefers it, so there's nothing for the probe to add and
     * we save a network round-trip. */
    const char *cur = getenv("HAX_CONTEXT_LIMIT");
    if (cur && *cur)
        return;
    const char *model = getenv("HAX_MODEL");
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

struct provider *openrouter_provider_new(void)
{
    /* Locked to openrouter.ai — HAX_OPENAI_BASE_URL is rejected so the
     * OPENROUTER_API_KEY fallback (and the public X-Title attribution
     * header) can never leak to an unrelated host. Custom OpenAI-compat
     * endpoints belong on HAX_PROVIDER=openai-compatible. */
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    if (base_env && *base_env) {
        fprintf(stderr, "hax: HAX_OPENAI_BASE_URL is not honored by HAX_PROVIDER=openrouter "
                        "(this preset is locked to openrouter.ai)\n"
                        "hax: use HAX_PROVIDER=openai-compatible to point at a custom endpoint\n");
        return NULL;
    }
    const char *key = getenv("HAX_OPENAI_API_KEY");
    if (!key || !*key)
        key = getenv("OPENROUTER_API_KEY");

    const char *title = getenv("HAX_OPENROUTER_TITLE");
    if (!title || !*title)
        title = "hax";
    const char *referer = getenv("HAX_OPENROUTER_REFERER");

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
    const char *show = getenv("HAX_SHOW_REASONING");
    int request_reasoning = show && *show && strcmp(show, "0") != 0;

    struct openai_preset preset = {
        .display_name = "openrouter",
        .default_base_url = "https://openrouter.ai/api/v1",
        .api_key_env = "OPENROUTER_API_KEY",
        .send_cache_key_default = 1,
        .extra_headers = headers,
        .reasoning_format = request_reasoning ? REASONING_NESTED : REASONING_FLAT,
    };
    /* Constructor copies headers internally, so the local strings can be
     * freed once it returns. */
    struct provider *p = openai_provider_new_preset(&preset);
    free(title_hdr);
    free(referer_hdr);
    if (p)
        spawn_context_probe(p, (key && *key) ? key : NULL);
    return p;
}

const struct provider_factory PROVIDER_OPENROUTER = {
    .name = "openrouter",
    .new = openrouter_provider_new,
};
