/* SPDX-License-Identifier: MIT */
#include "openai_compat.h"

#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "openai.h"
#include "util.h"

struct provider *openai_compat_provider_new(void)
{
    const char *base_env = config_str("openai.base_url");
    if (!base_env || !*base_env) {
        hax_err("HAX_PROVIDER=openai-compatible requires HAX_OPENAI_BASE_URL\n"
                "hax: e.g. HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1");
        return NULL;
    }
    struct openai_preset preset = {
        .display_name = "openai-compatible",
        /* No default_base_url: HAX_OPENAI_BASE_URL is mandatory and the
         * openai constructor reads it from env. */
        .send_cache_key_default = 0,
        /* OpenAI-compatible backends diverge on the reasoning wire
         * format (real OpenAI: flat; OpenRouter-like routers and some
         * proxies: nested). Let the user pick at runtime since we
         * don't know which one they're pointing at. */
        .reasoning_format =
            reasoning_format_parse(config_str("openai.reasoning_format"), REASONING_FLAT),
        /* A wildcard endpoint speaking the OpenAI dialect: offer the ladder
         * advisory (passed verbatim; effective only if the backend honors
         * it). The user picked the URL, so they know their backend. */
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    return openai_provider_new_preset(&preset);
}

/* Availability is simply "a base URL is configured" — we can't probe an
 * arbitrary third-party endpoint cheaply or know its auth scheme, so a set
 * HAX_OPENAI_BASE_URL is the signal. */
static int openai_compat_available(const char **reason)
{
    const char *base = config_str("openai.base_url");
    if (base && *base)
        return 1;
    if (reason)
        *reason = "HAX_OPENAI_BASE_URL not set";
    return 0;
}

const struct provider_factory PROVIDER_OPENAI_COMPAT = {
    .name = "openai-compatible",
    .new = openai_compat_provider_new,
    .available = openai_compat_available,
};
