/* SPDX-License-Identifier: MIT */
#include "openrouter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "busy.h"
#include "config.h"
#include "model_meta.h"
#include "openai.h"
#include "util.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "transport/http.h"

/* One entry's worth of metadata is a few KB, so this rarely takes more than
 * a second even on a slow link. Failure is silent — the lower tiers answer,
 * and HAX_CONTEXT_LIMIT remains a manual override. */
#define PROBE_TIMEOUT_S 5

static const char *openrouter_api_key(void);

/* ---------- /model picker metadata ---------- */

/* Does `arr` (a JSON array of strings) list `want`? A non-array — the field
 * is absent or a shape we don't recognize — is unknown, not a "no": the
 * picker must not report "no images" just because OpenRouter reshuffled its
 * catalog. */
static int cap_from_array(const json_t *arr, const char *want)
{
    if (!json_is_array(arr))
        return PROVIDER_CAP_UNKNOWN;
    for (size_t i = 0; i < json_array_size(arr); i++) {
        const char *s = json_string_value(json_array_get(arr, i));
        if (s && strcmp(s, want) == 0)
            return PROVIDER_CAP_YES;
    }
    return PROVIDER_CAP_NO;
}

/* OpenRouter quotes rates as USD-per-token decimal strings ("0.00001");
 * hax works in USD per 1M tokens throughout (see catalog.h), so scale up.
 * "-1" marks a model whose price is variable — the auto-routers — and must
 * read as unknown rather than free; "0" is a genuinely free model and does
 * survive as 0. */
static double openrouter_rate(const json_t *pricing, const char *key)
{
    const char *s = json_string_value(json_object_get(pricing, key));
    if (!s || !*s)
        return -1;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return -1;
    return v * 1e6;
}

/* The blurb's first line only: OpenRouter descriptions run to several
 * markdown paragraphs, and the picker's gutter is a few lines shared with
 * the metadata. Trailing whitespace trimmed; NULL when there's nothing. */
static char *openrouter_lead_line(const json_t *entry)
{
    const char *d = json_string_value(json_object_get(entry, "description"));
    if (!d)
        return NULL;
    size_t len = strcspn(d, "\r\n");
    while (len > 0 && (d[len - 1] == ' ' || d[len - 1] == '\t'))
        len--;
    return len ? xasprintf("%.*s", (int)len, d) : NULL;
}

/* Long-context tiers from `pricing.overrides` — replacement rates that
 * apply to the whole request once the prompt reaches min_prompt_tokens:
 *
 *     "overrides": [{"min_prompt_tokens": 200000, "prompt": "0.000006",
 *                    "completion": "0.0000225", "input_cache_read": ...}]
 *
 * The threshold carries over unchanged: both this and catalog_tier's
 * `above` are exclusive, describing the same upstream policy (Anthropic
 * bills the premium on prompts *exceeding* 200K), so the same model must
 * price identically whichever tier answered. An omitted rate stays
 * negative, which catalog_price already reads as "use the base rate". */
static void openrouter_parse_tiers(const json_t *pricing, struct model_info *out)
{
    json_t *overrides = json_object_get(pricing, "overrides");
    if (!json_is_array(overrides))
        return;
    size_t i;
    json_t *ov;
    json_array_foreach(overrides, i, ov)
    {
        if (out->n_tiers >= CATALOG_TIERS_MAX)
            break;
        if (!json_is_object(ov))
            continue;
        json_t *min = json_object_get(ov, "min_prompt_tokens");
        if (!json_is_integer(min) || json_integer_value(min) <= 0)
            continue;
        struct catalog_tier *t = &out->tiers[out->n_tiers++];
        t->above = (long)json_integer_value(min);
        t->cost_input = openrouter_rate(ov, "prompt");
        t->cost_output = openrouter_rate(ov, "completion");
        t->cost_cache_read = openrouter_rate(ov, "input_cache_read");
        t->cost_cache_write = openrouter_rate(ov, "input_cache_write");
        t->cost_cache_write_1h = openrouter_rate(ov, "input_cache_write_1h");
    }
}

void openrouter_parse_model(const json_t *entry, struct model_info *out)
{
    json_t *ctx = json_object_get(entry, "context_length");
    if (json_is_integer(ctx) && json_integer_value(ctx) > 0)
        out->context = (long)json_integer_value(ctx);

    /* The routed upstream's own cap on one response, where it declares one
     * (most of the catalog does). Distinct from context_length, which covers
     * the whole request. */
    json_t *top = json_object_get(entry, "top_provider");
    json_t *max_out = json_is_object(top) ? json_object_get(top, "max_completion_tokens") : NULL;
    if (json_is_integer(max_out) && json_integer_value(max_out) > 0)
        out->max_output = (long)json_integer_value(max_out);

    json_t *arch = json_object_get(entry, "architecture");
    out->image_input =
        cap_from_array(arch ? json_object_get(arch, "input_modalities") : NULL, "image");
    /* A model that can't be given tools can't run hax's loop at all, and a
     * fifth of the catalog is in that state (image/audio generators, the
     * moderation models, some chat-only endpoints). */
    out->tools = cap_from_array(json_object_get(entry, "supported_parameters"), "tools");

    json_t *pricing = json_object_get(entry, "pricing");
    if (json_is_object(pricing)) {
        out->cost_input = openrouter_rate(pricing, "prompt");
        out->cost_output = openrouter_rate(pricing, "completion");
        /* Roughly half the paid catalog quotes one; the rest simply don't
         * cache, which is itself worth not claiming either way. */
        out->cost_cache_read = openrouter_rate(pricing, "input_cache_read");
        out->cost_cache_write = openrouter_rate(pricing, "input_cache_write");
        out->cost_cache_write_1h = openrouter_rate(pricing, "input_cache_write_1h");
        openrouter_parse_tiers(pricing, out);
    }

    openrouter_parse_efforts(entry, &out->efforts);
    out->desc = openrouter_lead_line(entry);
}

/* Effort levels from the entry's `reasoning` block:
 *
 *     "reasoning": {"mandatory": false, "default_enabled": true,
 *                   "supported_efforts": ["max","xhigh","high","medium","low"],
 *                   "default_effort": "high"}
 *
 * Taken verbatim: OpenRouter normalizes every upstream's vocabulary into
 * this one field. It narrows hard — a third of the models listing efforts
 * stop at three levels — and since OpenRouter maps an unsupported level to
 * the nearest instead of failing, an un-narrowed picker would offer rows
 * that silently do nothing.
 *
 * No `reasoning` block is a definite "no levels" — a third of the catalog
 * can't reason at all — unless the entry lists the parameter anyway (see
 * below). A block without `supported_efforts` (a toggle or a token budget)
 * leaves the set unknown so the lower tiers can answer, as distinct from an
 * empty list, which denies every level. */
void openrouter_parse_efforts(const json_t *entry, struct effort_set *out)
{
    json_t *r = json_object_get(entry, "reasoning");
    if (!json_is_object(r)) {
        json_t *sp = json_object_get(entry, "supported_parameters");
        if (!json_is_array(sp))
            return;
        /* The router entries (openrouter/auto and friends) carry no
         * reasoning block — they describe no single model, having yet to
         * choose one — but do accept the parameter. Taking that as "no
         * levels" would hide /effort for them and drop a configured one, so
         * leave the question to the static ladder. Only an entry that
         * describes its parameters without any of these is a definite no. */
        for (size_t i = 0; i < json_array_size(sp); i++) {
            const char *s = json_string_value(json_array_get(sp, i));
            if (s && (strcmp(s, "reasoning") == 0 || strcmp(s, "reasoning_effort") == 0))
                return;
        }
        out->known = 1;
        return;
    }
    json_t *levels = json_object_get(r, "supported_efforts");
    if (!json_is_array(levels))
        return;
    out->known = 1; /* present-but-empty is an answer: no levels at all */
    for (size_t i = 0; i < json_array_size(levels); i++)
        effort_set_add(out, json_string_value(json_array_get(levels, i)));
}

/* Locate `model` in a `{"data": [ ... ]}` catalog page and hand the entry to
 * the same parser the /model picker uses.
 *
 * The exact-id match is load-bearing: the ?q= query below is a substring
 * search, so asking for "openai/gpt-5.6-sol" also returns
 * "openai/gpt-5.6-sol-pro" — and returns it first. Taking data[0] would
 * quietly describe a different (pricier) model than the one being used. */
void openrouter_parse_meta(const char *body, const char *model, struct model_info *out)
{
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return;
    json_t *data = json_object_get(root, "data");
    if (json_is_array(data)) {
        size_t i;
        json_t *entry;
        json_array_foreach(data, i, entry)
        {
            const char *id = json_string_value(json_object_get(entry, "id"));
            if (id && strcmp(id, model) == 0) {
                openrouter_parse_model(entry, out);
                break;
            }
        }
    }
    json_decref(root);
}

/* Percent-encode everything outside the RFC 3986 unreserved set. Model ids
 * carry '/' and ':' ("meta-llama/llama-3.2-3b-instruct:free"), which are
 * legal in a path segment but must not travel raw in a query. */
static char *query_escape(const char *s)
{
    static const char HEX[] = "0123456789ABCDEF";
    struct buf b;
    buf_init(&b);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            buf_append(&b, (const char *)p, 1);
        } else {
            char esc[3] = {'%', HEX[*p >> 4], HEX[*p & 0xf]};
            buf_append(&b, esc, 3);
        }
    }
    return buf_steal(&b);
}

/* Metadata for one model, from the catalog filtered by ?q=. The full
 * /models document is ~535 KB and the per-model /endpoints one carries
 * neither pricing nor effort levels; the filtered query answers in ~2.7 KB
 * with the whole entry. */
int openrouter_probe_model(struct provider *p, const char *model, struct model_probe *out)
{
    (void)p;
    if (!model || !*model)
        return -1;
    char *q = query_escape(model);
    out->url = xasprintf("https://openrouter.ai/api/v1/models?q=%s", q);
    free(q);
    const char *key = openrouter_api_key();
    if (key) {
        out->headers = xcalloc(2, sizeof(*out->headers));
        out->headers[0] = xasprintf("Authorization: Bearer %s", key);
        out->headers[1] = NULL;
    }
    out->timeout_s = PROBE_TIMEOUT_S;
    out->parse = openrouter_parse_meta;
    return 0;
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

struct provider *openrouter_provider_new(const char *name)
{
    (void)name;
    /* Fixed to openrouter.ai. HAX_OPENAI_BASE_URL is ignored (lock_base_url),
     * not rejected, so the OPENROUTER_API_KEY fallback and the attribution
     * headers can never reach an unrelated host, and a base URL
     * left set for another backend doesn't block selecting openrouter. Custom
     * endpoints belong on HAX_PROVIDER=openai-compatible. */
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

    struct openai_preset preset = {
        .display_name = "openrouter",
        .default_base_url = "https://openrouter.ai/api/v1",
        .api_key_env = "OPENROUTER_API_KEY",
        .send_cache_key_default = 1,
        .lock_base_url = 1,
        .request_cost = 1,
        /* Anthropic models here cache only on request, and OpenRouter
         * passes the marker through to them. */
        .send_cache_control_default = 1,
        .extra_headers = headers,
        /* The shared builder sends this object only when effort is set:
         * NULL keeps the provider default and "none" disables reasoning.
         * show_reasoning remains an independent display-only setting. */
        .reasoning_format = REASONING_NESTED,
        /* Safe as a vocabulary: OpenRouter maps an unsupported level to the
         * nearest rather than rejecting it. What each model actually takes
         * comes from openrouter_parse_efforts. */
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
        .parse_model = openrouter_parse_model,
    };
    /* Constructor copies headers internally, so the local strings can be
     * freed once it returns. */
    struct provider *p = openai_provider_new_preset(&preset);
    free(title_hdr);
    free(referer_hdr);
    if (p) {
        p->probe_model = openrouter_probe_model;
        p->query_usage = openrouter_query_usage;
        /* Hundreds of catalog entries in no meaningful order — alphabetize
         * so vendor prefixes group together in the picker. */
        p->sort_models = 1;
        model_meta_refresh(p, config_str("model"));
    }
    return p;
}

/* Usable iff a key is configured — HAX_OPENAI_API_KEY or the OPENROUTER_API_KEY
 * fallback the preset already consults. */
static void openrouter_prepare_availability(const char *name, struct provider_availability *out)
{
    (void)name;
    const char *reason = NULL;
    out->available =
        openai_key_available("OPENROUTER_API_KEY", "OPENROUTER_API_KEY not set", &reason);
    out->reason = reason;
}

const struct provider_factory PROVIDER_OPENROUTER = {
    .name = "openrouter",
    .new = openrouter_provider_new,
    .prepare_availability = openrouter_prepare_availability,
};
