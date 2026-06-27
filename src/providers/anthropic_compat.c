/* SPDX-License-Identifier: MIT */
#include "anthropic_compat.h"

#include <stdio.h>
#include <stdlib.h>

#include "anthropic.h"
#include "config.h"
#include "util.h"

struct provider *anthropic_compat_provider_new(const char *name)
{
    (void)name;
    const char *base = config_str("anthropic.base_url");
    if (!base || !*base) {
        hax_err("HAX_PROVIDER=anthropic-compatible requires HAX_ANTHROPIC_BASE_URL\n"
                "hax: e.g. HAX_ANTHROPIC_BASE_URL=http://127.0.0.1:18080/v1");
        return NULL;
    }

    /* No startup model probe: like the openai-compatible shim, leave the model
     * to HAX_MODEL or the /model picker (which fetches the catalog on demand).
     * Probing here would add unexpected latency for a cloud endpoint, and a
     * round-trip on every launch isn't worth saving one `HAX_MODEL=`. */
    struct anthropic_preset preset = {
        .display_name = "anthropic-compatible",
        /* No default_base_url: HAX_ANTHROPIC_BASE_URL is mandatory and the
         * constructor reads it from config. Deliberately no api_key_env so a
         * globally configured ANTHROPIC_API_KEY isn't forwarded to a
         * third-party endpoint. */
        .default_thinking_mode = ANTHROPIC_THINKING_BUDGET,
        /* Compat servers (llama-server) emit and accept empty thinking
         * signatures — round-trip them as-is rather than downgrading. */
        .allow_empty_signature = 1,
        /* A backend that rejects the cache_control field would 400, and we
         * can't know the target's support, so leave caching off by default
         * (opt in with HAX_ANTHROPIC_CACHE=1). */
        .send_cache_control_default = 0,
    };
    return anthropic_provider_new_preset(&preset);
}

static int anthropic_compat_available(const char *name, const char **reason)
{
    (void)name;
    const char *base = config_str("anthropic.base_url");
    if (base && *base)
        return 1;
    if (reason)
        *reason = "HAX_ANTHROPIC_BASE_URL not set";
    return 0;
}

const struct provider_factory PROVIDER_ANTHROPIC_COMPAT = {
    .name = "anthropic-compatible",
    .new = anthropic_compat_provider_new,
    .available = anthropic_compat_available,
};
