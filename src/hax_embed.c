/* SPDX-License-Identifier: MIT */
#include "hax_embed.h"

#include <stddef.h>
#include <curl/curl.h>

#include "agent_core.h"
#include "agent_loop.h"
#include "catalog.h"
#include "config.h"
#include "provider.h"
#include "session_prune.h"
#include "trace.h"
#include "transcript.h"
#include "util.h"
#include "providers/registry.h"
#include "system/tempfiles.h"
#include "terminal/theme.h"
#include "transport/ca.h"

static int initialized;
static int owns_curl_global;

int hax_init(const struct hax_embed_options *options)
{
    static const struct hax_embed_options defaults;
    const struct hax_embed_options *opts = options ? options : &defaults;

    if (initialized) {
        hax_err("hax_init: already initialized");
        return -1;
    }

    hax_set_diag_sink(opts->diag, opts->diag_user);
    if (!opts->own_atexit) {
        tempfiles_set_atexit_enabled(0);
        trace_set_atexit_enabled(0);
    }
    if (opts->own_locale)
        locale_init_utf8();

    /* Config-load diagnostics can honor terminal color, but not a theme in an unreadable file. */
    theme_set("auto");
    config_init();
    theme_init();

    trace_init();
    transcript_log_init();

    if (opts->own_curl_global) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            hax_err("curl_global_init failed");
            config_free();
            hax_set_diag_sink(NULL, NULL);
            return -1;
        }
        owns_curl_global = 1;
    }
    ca_init();

    initialized = 1;
    return 0;
}

void hax_shutdown(void)
{
    if (!initialized)
        return;
    initialized = 0;

    session_prune_shutdown();
    catalog_shutdown();
    if (owns_curl_global) {
        curl_global_cleanup();
        owns_curl_global = 0;
    }
    trace_close();
    tempfiles_cleanup();
    config_free();
    hax_set_diag_sink(NULL, NULL);
}

struct provider *hax_provider_new(const char *name)
{
    if (!name || !*name)
        name = config_str("provider");
    if (!name || !*name) {
        const struct provider_factory *fallback = provider_default();
        if (!fallback) {
            hax_err("no provider is available");
            return NULL;
        }
        return fallback->new(fallback->id);
    }

    const struct provider_factory *factory = provider_find(name);
    if (!factory) {
        hax_err("unknown provider '%s'", name);
        return NULL;
    }
    return factory->new(factory->id);
}

void hax_provider_destroy(struct provider *provider)
{
    if (provider)
        provider->destroy(provider);
}

const struct hax_abi *hax_abi(void)
{
    static const struct hax_abi abi = {
        .version = HAX_ABI_VERSION,
        .sizeof_item = sizeof(struct item),
        .sizeof_agent_session = sizeof(struct agent_session),
        .sizeof_agent_loop_params = sizeof(struct agent_loop_params),
        .sizeof_agent_loop_result = sizeof(struct agent_loop_result),
        .sizeof_agent_loop_hooks = sizeof(struct agent_loop_hooks),
    };
    return &abi;
}
