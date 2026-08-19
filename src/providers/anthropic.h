/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_H
#define HAX_PROVIDERS_ANTHROPIC_H

#include <jansson.h>

#include "provider.h"
#include "providers/anthropic_messages.h"

/* Configuration shared by providers using the Messages API. */
struct anthropic_preset {
    const char *display_name;     /* defaults to "anthropic" */
    const char *default_base_url; /* required when <prefix>.base_url does not resolve */
    const char *api_key_env;      /* fallback after the configured API key */
    const char *config_prefix;    /* config namespace; NULL reads no user configuration */
    int pin_base_url;             /* ignore <prefix>.base_url: a first-party key must not
                                     follow a configured URL to another host */
    const char *catalog_id;       /* copied; NULL disables catalog metadata */

    enum anthropic_thinking_mode default_thinking_mode;
    int allow_empty_signature;      /* preserve unsigned thinking blocks on compat backends */
    int send_cache_control_default; /* overridable by the cache setting */
};

/* Preset strings need only remain valid during construction. */
struct provider *anthropic_provider_new_preset(const struct anthropic_preset *preset);

/* Construct the api.anthropic.com provider; `id` becomes its stable identity. */
struct provider *anthropic_provider_new(const char *id);

/* Adaptive-thinking effort values ordered from cheapest to most expensive. */
extern const char *const ANTHROPIC_EFFORT_LADDER[];
extern const size_t ANTHROPIC_EFFORT_LADDER_N;

/* `out` is initialized and already owns the entry's id. */
void anthropic_parse_model(const json_t *entry, struct model_info *out);

/* Return the configured output cap, clamped to reported model metadata when available. */
int anthropic_max_tokens(struct provider *provider, const char *model);

extern const struct provider_factory PROVIDER_ANTHROPIC;

#endif /* HAX_PROVIDERS_ANTHROPIC_H */
