/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_HTTP_PROVIDER_H
#define HAX_PROVIDERS_HTTP_PROVIDER_H

#include <jansson.h>

#include "provider.h"
#include "providers/anthropic_body.h"
#include "providers/chat_body.h"
#include "providers/wire.h"

/* Generic streaming-HTTP provider: endpoint, credential, and configuration resolution, request
 * policy, and stream mechanics for any wire dialect. Concrete providers are presets over this
 * core; modules with their own metadata protocol override the relevant provider callbacks after
 * construction, using the accessors below. */

struct http_provider_preset {
    const char *display_name;     /* required */
    const char *default_base_url; /* required when <prefix>.base_url does not resolve */
    const char *api_key_env;      /* fallback after the configured API key */
    const char *config_prefix;    /* config namespace; NULL reads no user configuration */
    int pin_base_url;             /* ignore <prefix>.base_url: a first-party key must not
                                     follow a configured URL to another host */
    const char *catalog_id;       /* copied; NULL disables catalog metadata */
    /* Default request protocol; NULL means Chat Completions. An OpenAI-family choice may be
     * overridden by <prefix>.api; the Messages wire is pinned because its knobs differ. */
    const struct wire *wire;
    /* Resolve each model's wire from the catalog api hint: for gateways serving models over
     * different protocols behind one base URL. <prefix>.model_apis rules take precedence and
     * work without this flag; unmatched models fall back to the default wire. */
    int catalog_wires;

    /* Chat Completions / Responses policy. */
    int send_cache_key_default;
    int cache_auto_default; /* AUTO when set, otherwise OFF */
    int emit_progress;      /* request and parse llama.cpp prompt_progress */
    int request_cost;       /* request OpenRouter usage cost */
    enum chat_reasoning_format reasoning_format;
    const char *reasoning_replay_field; /* copied; NULL disables replay */

    /* Messages policy. */
    enum anthropic_thinking_mode default_thinking_mode;
    int allow_empty_signature;      /* preserve unsigned thinking blocks on compat backends */
    int send_cache_control_default; /* overridable by the cache setting */
    const char *default_version;    /* anthropic-version; NULL falls back to 2023-06-01 */

    /* Vertex-style serving: base_url names a complete stream endpoint (a Google
     * `...:streamRawPredict` URL) so no wire path is appended. */
    int raw_endpoint;
    /* Return the bearer token for a request; borrowed, may be NULL. Only consulted when the
     * wire is Messages; NULL falls back to the configured API key behind a `Bearer ` header. */
    const char *(*bearer_token)(const struct provider *provider);

    const char *const *extra_headers; /* NULL-terminated; copied */

    /* Borrowed for the provider lifetime. NULL/0 disables the effort picker. On the Messages
     * wire the list is offered only while adaptive thinking is active. */
    const char *const *efforts;
    size_t n_efforts;
    const char *length_hint; /* borrowed for the provider lifetime */

    /* `out` is initialized and already owns the entry's id. */
    void (*parse_model)(const json_t *entry, struct model_info *out);
};

/* Preset strings need only remain valid during construction unless marked borrowed above. */
struct provider *http_provider_new_preset(const struct http_provider_preset *preset);

/* Accessors for preset modules implementing their own metadata callbacks. */
const char *http_provider_base_url(const struct provider *provider);
int http_provider_has_api_key(const struct provider *provider);
/* The resolved key, or NULL; borrowed for the provider's lifetime. */
const char *http_provider_api_key(const struct provider *provider);
/* Owned NULL-terminated auth headers for JSON metadata requests, including the version header
 * on the Messages wire; free with string_array_free. */
char **http_provider_metadata_headers(const struct provider *provider);

/* Output cap for one Messages request: <prefix>.max_tokens clamped to model metadata. */
int http_provider_max_tokens(struct provider *provider, const char *model);

/* Populate an owned GET <base_url>/models availability request. `extra_headers` (may be NULL)
 * are copied after the Authorization header. */
void http_provider_prepare_base_url_availability(const char *base_url, const char *api_key,
                                                 char *const *extra_headers,
                                                 struct provider_availability *out);

#endif /* HAX_PROVIDERS_HTTP_PROVIDER_H */
