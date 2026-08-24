/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_HTTP_PROVIDER_H
#define HAX_PROVIDERS_HTTP_PROVIDER_H

#include <jansson.h>

#include "provider.h"
#include "providers/chat_body.h"
#include "providers/wire.h"

/* Generic streaming-HTTP provider: endpoint, credential, and configuration resolution, request
 * policy, and stream mechanics for any wire dialect. Concrete providers are presets over this
 * core; modules with their own metadata protocol override the relevant provider callbacks after
 * construction, using the accessors below. */

/* Per-entry /models refinement: fill capabilities and pricing from one listing entry. `out` is
 * initialized and already owns the entry's id. */
typedef void (*http_parse_model_cb)(const json_t *entry, struct model_info *out);

/* The dialect of the provider's model-metadata side — the /models listing and probe, and the
 * auth scheme those requests use. A property of the endpoint, not of the per-model request
 * wire: a mixed-protocol gateway serves one catalog shape however each model is spoken to. */
enum http_metadata_api {
    HTTP_METADATA_BY_WIRE = 0, /* follow the default wire's family */
    HTTP_METADATA_OPENAI,      /* flat {"data": [...]} list, Bearer auth */
    HTTP_METADATA_ANTHROPIC,   /* cursor-paginated list, x-api-key + version auth */
};

struct http_provider_preset {
    const char *display_name;     /* required */
    const char *default_base_url; /* required when <prefix>.base_url does not resolve */
    const char *api_key_env;      /* fallback after the configured API key */
    const char *config_prefix;    /* config namespace; NULL reads no user configuration */
    int pin_base_url;             /* ignore <prefix>.base_url: a first-party key must not
                                     follow a configured URL to another host */
    const char *catalog_id;       /* default under <prefix>.catalog_id; copied; NULL disables
                                     catalog metadata (an explicit empty config value opts out) */
    /* Default request protocol; NULL means Chat Completions. <prefix>.api may move an unpinned
     * OpenAI-family choice between the two OpenAI protocols; pinned endpoints and the Messages
     * wire (whose knobs differ) are fixed. */
    const struct wire *wire;
    enum http_metadata_api metadata_api;
    /* Resolve each model's wire from the catalog api hint: for gateways serving models over
     * different protocols behind one base URL. <prefix>.model_apis rules take precedence and
     * work without this flag; unmatched models fall back to the default wire. */
    int catalog_wires;

    /* Cache-marker default under <prefix>.cache: "auto" plans chat markers from model rates,
     * "on" always sends them (the Messages side knows only on/off); NULL sends none. */
    const char *cache;

    /* Chat Completions / Responses policy. */
    int send_cache_key_default;
    int emit_progress; /* request and parse llama.cpp prompt_progress */
    int request_cost;  /* request OpenRouter usage cost */
    enum chat_reasoning_format reasoning_format;
    const char *reasoning_replay_field; /* copied; NULL disables replay */

    /* Messages policy. */
    const char *thinking_mode; /* default mode under <prefix>.thinking_mode; NULL → budget */
    int strict_signatures;     /* the endpoint validates thinking-block signatures, so unsigned
                                  blocks are dropped rather than replayed and rejected */

    const char *const *extra_headers; /* NULL-terminated; copied */

    /* Borrowed for the provider lifetime. NULL/0 disables the effort picker. On the Messages
     * wire the list is offered only while adaptive thinking is active. */
    const char *const *efforts;
    size_t n_efforts;
    const char *length_hint; /* borrowed for the provider lifetime */

    http_parse_model_cb parse_model;
};

/* Preset strings need only remain valid during construction unless marked borrowed above. */
struct provider *http_provider_new_preset(const struct http_provider_preset *preset);

/* Accessors for preset modules implementing their own metadata callbacks. */
const char *http_provider_base_url(const struct provider *provider);
int http_provider_has_api_key(const struct provider *provider);
/* The resolved key, or NULL; borrowed for the provider's lifetime. */
const char *http_provider_api_key(const struct provider *provider);
/* Owned NULL-terminated auth headers for JSON metadata requests, following the resolved
 * metadata dialect (x-api-key plus the version header on the Anthropic side); free with
 * string_array_free. */
char **http_provider_metadata_headers(const struct provider *provider);
/* The preset's per-entry /models refinement hook, or NULL. */
http_parse_model_cb http_provider_parse_model(const struct provider *provider);
/* The metadata dialect the constructor resolved and installed. */
enum http_metadata_api http_provider_metadata_api(const struct provider *provider);

/* Output cap for one Messages request: <prefix>.max_tokens clamped to model metadata. */
int http_provider_max_tokens(struct provider *provider, const char *model);

/* Populate an owned GET <base_url>/models availability request. `extra_headers` (may be NULL)
 * are copied after the Authorization header. */
void http_provider_prepare_base_url_availability(const char *base_url, const char *api_key,
                                                 char *const *extra_headers,
                                                 struct provider_availability *out);

#endif /* HAX_PROVIDERS_HTTP_PROVIDER_H */
