/* SPDX-License-Identifier: MIT */
#include "providers/openai.h"

#include <stddef.h>

#include "provider.h"
#include "providers/config_provider.h"
#include "providers/http_provider.h"
#include "providers/openai_messages.h"
#include "providers/wire.h"

struct provider *openai_provider_new(const char *id)
{
    provider_warn_unused_wire_fields(id, &WIRE_OPENAI_RESPONSES, NULL);
    /* Tweaks resolve from the provider's own block; the pinned endpoint keeps OPENAI_API_KEY
     * from being redirected to a custom URL. */
    struct http_provider_preset preset = {
        .display_name = "openai",
        .default_base_url = "https://api.openai.com/v1",
        .api_key_env = "OPENAI_API_KEY",
        .config_prefix = "providers.openai",
        .pin_base_url = 1,
        .send_cache_key_default = 1,
        .wire = &WIRE_OPENAI_RESPONSES,
        .catalog_id = "openai",
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    if (provider) {
        provider->id = id;
        provider->sort_models = 1;
    }
    return provider;
}

static void openai_prepare_availability(const char *id, struct provider_availability *out)
{
    (void)id;
    out->available = provider_api_key("providers.openai", "OPENAI_API_KEY") != NULL;
    out->reason = out->available ? NULL : "OPENAI_API_KEY not set";
}

const struct provider_factory PROVIDER_OPENAI = {
    .id = "openai",
    .new = openai_provider_new,
    .prepare_availability = openai_prepare_availability,
};
