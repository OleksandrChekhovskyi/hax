/* SPDX-License-Identifier: MIT */
#include "openai_compat.h"

#include <stdio.h>
#include <stdlib.h>

#include "openai.h"

struct provider *openai_compat_provider_new(void)
{
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    if (!base_env || !*base_env) {
        fprintf(stderr, "hax: HAX_PROVIDER=openai-compatible requires HAX_OPENAI_BASE_URL\n"
                        "hax: e.g. HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1\n");
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
            reasoning_format_parse(getenv("HAX_OPENAI_REASONING_FORMAT"), REASONING_FLAT),
    };
    return openai_provider_new_preset(&preset);
}

const struct provider_factory PROVIDER_OPENAI_COMPAT = {
    .name = "openai-compatible",
    .new = openai_compat_provider_new,
};
