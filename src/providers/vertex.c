/* SPDX-License-Identifier: MIT */
#include "providers/vertex.h"

#include <stdlib.h>

#include "config.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic_body.h"
#include "providers/http_provider.h"
#include "providers/vertex_auth.h"
#include "providers/wire.h"

/* Called by the shared Messages stream loop / metadata headers to authenticate every request. */
static const char *vertex_bearer_token(const struct provider *base)
{
    (void)base;
    char *error = NULL;
    const char *token = vertex_auth_token(&error);
    if (!token)
        hax_warn("%s", error);
    free(error);
    return token;
}

struct provider *vertex_provider_new(const char *name)
{
    (void)name;
    const char *base = config_str("vertex.base_url");
    if (!base || !*base) {
        hax_err("HAX_PROVIDER=vertex requires HAX_VERTEX_BASE_URL naming a Vertex "
                "...:streamRawPredict endpoint");
        return NULL;
    }

    struct http_provider_preset preset = {
        .display_name = "vertex",
        .config_prefix = "vertex",
        .default_thinking_mode = ANTHROPIC_THINKING_BUDGET,
        .allow_empty_signature = 1,
        .send_cache_control_default = 0,
        .default_version = "vertex-2023-10-16",
        .raw_endpoint = 1,
        .bearer_token = vertex_bearer_token,
        .wire = &WIRE_ANTHROPIC_MESSAGES,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    if (!provider)
        return NULL;
    /* The endpoint URL is the per-model stream path; there is nothing to enumerate. */
    provider->list_models = NULL;
    provider->probe_model = NULL;
    return provider;
}

static void vertex_prepare_availability(const char *name, struct provider_availability *out)
{
    (void)name;
    const char *base = config_str("vertex.base_url");
    const char *reason = NULL;
    out->available = base && *base && vertex_auth_available(&reason);
    if (!out->available)
        out->reason = xstrdup(base && *base ? reason : "HAX_VERTEX_BASE_URL not set");
}

const struct provider_factory PROVIDER_VERTEX = {
    .id = "vertex",
    .new = vertex_provider_new,
    .prepare_availability = vertex_prepare_availability,
};
