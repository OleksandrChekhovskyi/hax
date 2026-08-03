/* SPDX-License-Identifier: MIT */
#include "providers/openai_compat.h"

#include <stddef.h>

#include "config.h"
#include "provider.h"
#include "util.h"
#include "providers/openai.h"
#include "providers/openai_messages.h"

struct provider *openai_compat_provider_new(const char *name)
{
    (void)name;

    const char *base_url = config_str("openai.base_url");
    if (!base_url || !*base_url) {
        hax_err("HAX_PROVIDER=openai-compatible requires HAX_OPENAI_BASE_URL\n"
                "hax: e.g. HAX_OPENAI_BASE_URL=http://127.0.0.1:8000/v1");
        return NULL;
    }

    struct openai_preset preset = {
        .display_name = "openai-compatible",
        .reasoning_format = openai_reasoning_format_parse(config_str("openai.reasoning_format"),
                                                          OPENAI_REASONING_FLAT),
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    return openai_provider_new_preset(&preset);
}

static void openai_compat_prepare_availability(const char *name, struct provider_availability *out)
{
    (void)name;
    const char *base_url = config_str("openai.base_url");
    out->available = base_url && *base_url;
    out->reason = out->available ? NULL : "HAX_OPENAI_BASE_URL not set";
}

const struct provider_factory PROVIDER_OPENAI_COMPAT = {
    .name = "openai-compatible",
    .new = openai_compat_provider_new,
    .prepare_availability = openai_compat_prepare_availability,
};
