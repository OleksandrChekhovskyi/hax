/* SPDX-License-Identifier: MIT */
#include "openrouter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "busy.h"
#include "config.h"
#include "openai.h"
#include "probe.h"
#include "util.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "transport/http.h"

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

/* /usage: what OpenRouter knows about the key we're spending, from two
 * endpoints (both auth'd by the key itself, so they always match the
 * account we stream against):
 *
 *   GET /key — key-scoped: { "data": { "label": ..., "usage": 1.23,
 *     "limit": 10.0|null, "limit_remaining": 8.77|null,
 *     "is_free_tier": false, ... } }. `limit` is a per-key spending cap;
 *     null (the default — most keys are uncapped) skips the row rather
 *     than showing $0.
 *
 *   GET /credits — account-scoped: { "data": { "total_credits": 10.0,
 *     "total_usage": 0.007 } }. remaining = credits - usage is the
 *     "how much money is left" number most users are after, which the
 *     key endpoint alone can't answer. Fetch failure just drops the row.
 *
 * All dollar amounts are plain numbers. */
#define OPENROUTER_KEY_ENDPOINT     "https://openrouter.ai/api/v1/key"
#define OPENROUTER_CREDITS_ENDPOINT "https://openrouter.ai/api/v1/credits"
#define USAGE_LABEL_W               11

static int openrouter_query_usage(struct provider *p)
{
    (void)p;
    const char *key = openrouter_api_key();
    if (!key) {
        ui_error("no OpenRouter API key configured");
        return -1;
    }
    char *auth_hdr = xasprintf("Authorization: Bearer %s", key);
    const char *headers[] = {auth_hdr, "Accept: application/json", NULL};

    /* Same UX as the codex /usage fetch: bounded round-trips under a busy
     * window — spinner + Esc cancel, both no-ops on non-TTY stdout. */
    struct busy *b = busy_begin("fetching usage...");
    char *body = NULL, *credits_body = NULL;
    long status = 0;
    int rc = http_get(OPENROUTER_KEY_ENDPOINT, headers, 30, 0, busy_tick, NULL, &body, &status);
    if (rc == 0)
        http_get(OPENROUTER_CREDITS_ENDPOINT, headers, 30, 0, busy_tick, NULL, &credits_body, NULL);
    int cancelled = busy_end(b);
    free(auth_hdr);
    if (cancelled) {
        /* User abandoned the wait — busy_end left the [interrupted]
         * marker; not a failure, no diagnostic. */
        free(body);
        free(credits_body);
        return -1;
    }
    if (rc != 0 || !body) {
        if (status == 401)
            ui_error("OpenRouter rejected the API key (401) — check OPENROUTER_API_KEY");
        else
            ui_error("failed to fetch usage from %s", OPENROUTER_KEY_ENDPOINT);
        free(body);
        free(credits_body);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(body, 0, &jerr);
    free(body);
    if (!root) {
        ui_error("usage response is not valid JSON: %s", jerr.text);
        free(credits_body);
        return -1;
    }
    json_t *data = json_object_get(root, "data");
    if (!json_is_object(data)) {
        ui_error("unrecognized usage response shape (no data object)");
        json_decref(root);
        free(credits_body);
        return -1;
    }

    const char *label = json_string_value(json_object_get(data, "label"));
    printf(ANSI_DIM "openrouter");
    /* OpenRouter defaults a key's label to its masked form
     * ("sk-or-v1-1a2...b3c"). Even truncated, key-shaped material doesn't
     * belong in scrollback (screenshots, pasted output, transcript
     * recordings) — and a masked default identifies nothing anyway. Show
     * the label only when it's a human-chosen key name. */
    if (label && *label && strncmp(label, "sk-", 3) != 0)
        printf(" · %s", label);
    if (json_is_true(json_object_get(data, "is_free_tier")))
        printf(" · free tier");
    printf(ANSI_RESET "\n");

    char amount[32];
    json_t *v = json_object_get(data, "usage");
    if (json_is_number(v)) {
        format_cost(amount, sizeof(amount), json_number_value(v));
        printf("  " ANSI_DIM "%-*s%s" ANSI_RESET "\n", USAGE_LABEL_W, "spent", amount);
    }
    v = json_object_get(data, "limit");
    if (json_is_number(v)) {
        format_cost(amount, sizeof(amount), json_number_value(v));
        printf("  " ANSI_DIM "%-*s%s", USAGE_LABEL_W, "key limit", amount);
        json_t *rem = json_object_get(data, "limit_remaining");
        if (json_is_number(rem)) {
            format_cost(amount, sizeof(amount), json_number_value(rem));
            printf(" · %s remaining", amount);
        }
        printf(ANSI_RESET "\n");
    }
    json_decref(root);

    /* Account credits. Skipped when nothing was ever purchased
     * (total_credits <= 0 — BYOK/free accounts, where "remaining $0" would
     * read as an alarm rather than information). Overspend past the
     * prepaid balance clamps to $0.00. */
    if (credits_body) {
        json_t *croot = json_loads(credits_body, 0, NULL);
        json_t *cdata = croot ? json_object_get(croot, "data") : NULL;
        json_t *tc = json_is_object(cdata) ? json_object_get(cdata, "total_credits") : NULL;
        json_t *tu = json_is_object(cdata) ? json_object_get(cdata, "total_usage") : NULL;
        if (json_is_number(tc) && json_is_number(tu) && json_number_value(tc) > 0) {
            double remaining = json_number_value(tc) - json_number_value(tu);
            if (remaining < 0)
                remaining = 0;
            char total[32];
            format_cost(amount, sizeof(amount), remaining);
            format_cost(total, sizeof(total), json_number_value(tc));
            printf("  " ANSI_DIM "%-*s%s of %s remaining" ANSI_RESET "\n", USAGE_LABEL_W, "credits",
                   amount, total);
        }
        json_decref(croot);
    }
    free(credits_body);
    return 0;
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

struct provider *openrouter_provider_new(const char *name)
{
    (void)name;
    /* Fixed to openrouter.ai. HAX_OPENAI_BASE_URL is ignored (lock_base_url),
     * not rejected, so the OPENROUTER_API_KEY fallback and the attribution
     * headers can never reach an unrelated host, and a base URL
     * left set for another backend doesn't block selecting openrouter. Custom
     * endpoints belong on HAX_PROVIDER=openai-compatible. */
    const char *key = openrouter_api_key();

    const char *title = config_str("openrouter.title");
    const char *referer = config_str("openrouter.referer");

    /* OpenRouter attributes usage to an app by HTTP-Referer — without it no
     * app page exists and traffic shows as "Unknown"; X-Title merely labels
     * that page and does nothing on its own. Both default via the config
     * registry, and an explicit empty value opts out of attribution. The
     * categories header only refines an app page, so it rides with the
     * referer. */
    char *title_hdr = (title && *title) ? xasprintf("X-Title: %s", title) : NULL;
    char *referer_hdr = (referer && *referer) ? xasprintf("HTTP-Referer: %s", referer) : NULL;

    const char *headers[4];
    size_t i = 0;
    if (title_hdr)
        headers[i++] = title_hdr;
    if (referer_hdr) {
        headers[i++] = referer_hdr;
        headers[i++] = "X-OpenRouter-Categories: cli-agent";
    }
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
        .request_cost = 1,
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
        p->query_usage = openrouter_query_usage;
        /* Hundreds of catalog entries in no meaningful order — alphabetize
         * so vendor prefixes group together in the picker. */
        p->sort_models = 1;
        spawn_context_probe(p, key);
    }
    return p;
}

/* Usable iff a key is configured — HAX_OPENAI_API_KEY or the OPENROUTER_API_KEY
 * fallback the preset already consults. */
static int openrouter_available(const char *name, const char **reason)
{
    (void)name;
    return openai_key_available("OPENROUTER_API_KEY", "OPENROUTER_API_KEY not set", reason);
}

const struct provider_factory PROVIDER_OPENROUTER = {
    .name = "openrouter",
    .new = openrouter_provider_new,
    .available = openrouter_available,
};
