/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_compat.h"

#include <stdlib.h>

#include "config.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic.h"

struct provider *anthropic_compat_provider_new(const char *name)
{
    (void)name;
    const char *base = config_str("anthropic.base_url");
    if (!base || !*base) {
        hax_err("HAX_PROVIDER=anthropic-compatible requires HAX_ANTHROPIC_BASE_URL\n"
                "hax: e.g. HAX_ANTHROPIC_BASE_URL=http://127.0.0.1:18080/v1");
        return NULL;
    }

    struct anthropic_preset preset = {
        .display_name = "anthropic-compatible",
        .default_thinking_mode = ANTHROPIC_THINKING_BUDGET,
        .allow_empty_signature = 1,
    };
    return anthropic_provider_new_preset(&preset);
}

static void anthropic_compat_prepare_availability(const char *name,
                                                  struct provider_availability *out)
{
    (void)name;
    const char *base = config_str("anthropic.base_url");
    out->available = base && *base;
    out->reason = out->available ? NULL : "HAX_ANTHROPIC_BASE_URL not set";
}

const struct provider_factory PROVIDER_ANTHROPIC_COMPAT = {
    .name = "anthropic-compatible",
    .new = anthropic_compat_provider_new,
    .prepare_availability = anthropic_compat_prepare_availability,
};
