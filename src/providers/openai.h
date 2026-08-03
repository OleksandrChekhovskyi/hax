/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_H
#define HAX_PROVIDERS_OPENAI_H

#include <jansson.h>

#include "provider.h"
#include "providers/openai_messages.h"

/* Construct the api.openai.com provider. `name` is unused. */
struct provider *openai_provider_new(const char *name);

/* Configuration shared by providers using the Chat Completions protocol. */
struct openai_preset {
    const char *display_name;     /* defaults to "openai" */
    const char *default_base_url; /* required when base_url is locked or not configured */
    const char *api_key_env;      /* fallback after the configured API key */
    const char *config_prefix;    /* NULL uses the global openai.* namespace */
    const char *catalog_id;       /* copied; NULL disables catalog metadata */
    int lock_base_url;            /* ignore configured base_url */

    int send_cache_key_default;
    int cache_auto_default; /* AUTO when set, otherwise OFF */
    int emit_progress;      /* request and parse llama.cpp prompt_progress */
    int request_cost;       /* request OpenRouter usage cost */
    enum openai_reasoning_format reasoning_format;
    const char *reasoning_replay_field; /* copied; NULL disables replay */
    const char *const *extra_headers;   /* NULL-terminated; copied */

    /* Borrowed for the provider lifetime. NULL/0 disables the effort picker. */
    const char *const *efforts;
    size_t n_efforts;
    const char *length_hint; /* borrowed for the provider lifetime */

    /* `out` is initialized and already owns the entry's id. */
    void (*parse_model)(const json_t *entry, struct model_info *out);
};

/* Shared effort vocabulary, ordered from cheapest to most expensive. */
extern const char *const OPENAI_EFFORT_LADDER[];
extern const size_t OPENAI_EFFORT_LADDER_N;

/* Preset strings need only remain valid during construction unless marked borrowed above. */
struct provider *openai_provider_new_preset(const struct openai_preset *preset);

int openai_key_available(const char *api_key_env, const char *missing_reason, const char **reason);

/* Populate an owned GET <base_url>/models availability request. */
void openai_prepare_base_url_availability(const char *base_url, const char *api_key,
                                          struct provider_availability *out);

enum openai_cache_mode {
    OPENAI_CACHE_OFF,
    OPENAI_CACHE_AUTO,
    OPENAI_CACHE_ON,
};

struct openai_cache_plan {
    int send_breakpoints;
    int writes_bill_1h;
};

/* Derive request and billing behavior from the selected model's cache rates. */
struct openai_cache_plan openai_plan_cache(const struct provider *provider, const char *model,
                                           enum openai_cache_mode mode, const char *ttl);

extern const struct provider_factory PROVIDER_OPENAI;

#endif /* HAX_PROVIDERS_OPENAI_H */
