/* SPDX-License-Identifier: MIT */
#include "provider_router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "agent_core.h"
#include "config.h"
#include "diag.h"
#include "model_meta.h"
#include "providers/registry.h"
#include "transport/retry.h"
#include "xalloc.h"

/* Internal provider struct that wraps the router */
struct provider_router_provider {
    struct provider base;
    struct provider_router *router;
};

struct provider_router {
    struct provider **providers;
    size_t n_providers;
    size_t current_index;
    const char **provider_ids;  /* for display/logging */
};

static void router_provider_destroy(struct provider *provider) {
    struct provider_router_provider *rpp = (struct provider_router_provider *)provider;
    provider_router_destroy(rpp->router);
    free(rpp);
}

struct provider_router *provider_router_new(const char *const *provider_ids, size_t count) {
    struct provider_router *router = xcalloc(1, sizeof(*router));
    router->providers = xcalloc(count, sizeof(*router->providers));
    router->provider_ids = xcalloc(count, sizeof(*router->provider_ids));
    router->n_providers = count;
    
    for (size_t i = 0; i < count; i++) {
        router->provider_ids[i] = xstrdup(provider_ids[i]);
        const struct provider_def *def = provider_find(provider_ids[i]);
        if (def) {
            router->providers[i] = provider_construct(def);
            if (!router->providers[i]) {
                /* Cleanup on partial failure */
                for (size_t j = 0; j < i; j++) {
                    if (router->providers[j])
                        router->providers[j]->destroy(router->providers[j]);
                }
                provider_router_destroy(router);
                return NULL;
            }
        } else {
            /* Provider not found - will be skipped */
            router->providers[i] = NULL;
        }
    }
    return router;
}

void provider_router_destroy(struct provider_router *router) {
    if (!router) return;
    for (size_t i = 0; i < router->n_providers; i++) {
        if (router->providers[i])
            router->providers[i]->destroy(router->providers[i]);
        free((void *)router->provider_ids[i]);
    }
    free(router->providers);
    free(router->provider_ids);
    free(router);
}

struct provider *provider_router_active(const struct provider_router *router) {
    if (!router || router->current_index >= router->n_providers) return NULL;
    return router->providers[router->current_index];
}

int provider_is_router(const struct provider *provider) {
    return provider && provider->destroy == router_provider_destroy;
}

int provider_router_position(const struct provider *provider, size_t *out_current, size_t *out_total) {
    if (!provider_is_router(provider)) return 0;
    struct provider_router_provider *rpp = (struct provider_router_provider *)provider;
    struct provider_router *router = rpp->router;
    if (router->current_index >= router->n_providers) return 0;
    if (out_current) *out_current = router->current_index + 1;
    if (out_total) *out_total = router->n_providers;
    return 1;
}


struct wrap_ctx {
    stream_cb original;
    void *user;
    struct stream_event *captured;
    int *flag;
};
static struct wrap_ctx *wrap_state;

static int wrapper_callback(const struct stream_event *ev, void *ud) {
    (void)ud;
    if (ev->kind == EV_ERROR) {
        *wrap_state->captured = *ev;
        *wrap_state->flag = 1;
    }
    return wrap_state->original(ev, wrap_state->user);
}
/* Stream implementation with fallback logic */
int provider_router_stream(struct provider *provider, const struct context *context,
                           const char *model, stream_cb callback, void *user,
                           http_tick_cb tick, void *tick_user) {
    struct provider_router_provider *rpp = (struct provider_router_provider *)provider;
    struct provider_router *router = rpp->router;
    
    while (router->current_index < router->n_providers) {
        struct provider *current = router->providers[router->current_index];
        const char *current_id = router->provider_ids[router->current_index];
        
        if (!current) {
            router->current_index++;
            continue;
        }
        
    struct stream_event captured_error = {0};
    int error_captured = 0;
    struct wrap_ctx wrap_local = {callback, user, &captured_error, &error_captured};
    wrap_state = &wrap_local;
        
        int rc = current->stream(current, context, model, wrapper_callback, user, tick, tick_user);
        
        /* If no terminal error, we're done */
        if (!error_captured || !retry_is_terminal_error(captured_error.u.error.http_status,
                                                         captured_error.u.error.message)) {
            return rc;
        }
        
        /* Terminal error - check if we can fall back */
        if (router->current_index + 1 >= router->n_providers) {
            /* Last provider - return the error */
            return rc;
        }
        
        /* Emit user-visible note about fallback */
        hax_warn("provider '%s' exhausted (%s) — falling back to '%s'",
                 current_id, captured_error.u.error.message,
                 router->provider_ids[router->current_index + 1]);
        
        /* Drop reasoning for the next provider.
         * The session items already contain reasoning items; they will be dropped
         * by the agent loop when it rebuilds context for the next turn. */
        
        /* Move to next provider */
        router->current_index++;
    }
    
    return -1;
}

/* Exported helper to build router from config */
struct provider *provider_router_construct_from_config(void) {
    /* Get list of provider ids from config in order */
    char **names = NULL;
    size_t n_names = config_object_keys("providers", &names);
    if (n_names == 0) return NULL;
    
    /* Filter to only available/configured providers */
    const char **valid_ids = xcalloc(n_names, sizeof(*valid_ids));
    size_t valid_count = 0;
    
    for (size_t i = 0; i < n_names; i++) {
        const struct provider_def *def = provider_find(names[i]);
        if (def) {
            /* Check if provider has required config */
            char *key = xasprintf("providers.%s.base_url", names[i]);
            const char *url = config_str(key);
            free(key);
            
            /* For pinned providers, they're always available */
            if (def->pinned || (url && *url)) {
                valid_ids[valid_count++] = names[i];
            } else {
                free(names[i]);
            }
        } else {
            free(names[i]);
        }
    }
    free(names);
    
    if (valid_count == 0) {
        free(valid_ids);
        return NULL;
    }
    
    struct provider_router *router = provider_router_new(valid_ids, valid_count);
    free(valid_ids);
    
    if (!router) return NULL;
    
    /* Wrap in provider struct */
    struct provider_router_provider *rpp = xcalloc(1, sizeof(*rpp));
    rpp->router = router;
    rpp->base.name = "fallback-router";
    rpp->base.id = "fallback";
    rpp->base.stream = provider_router_stream;
    rpp->base.destroy = router_provider_destroy;
    rpp->base.default_model = NULL;
    rpp->base.model_label = NULL;
    rpp->base.default_effort = NULL;
    rpp->base.model_discovered = 0;
    rpp->base.keep_model_order = 0;
    rpp->base.catalog_id = NULL;
    rpp->base.list_models = NULL;
    rpp->base.list_efforts = NULL;
    rpp->base.probe_model = NULL;
    rpp->base.query_usage = NULL;
    rpp->base.meta = NULL;
    
    /* Set first provider as active for model resolution */
    config_set_override("provider", router->provider_ids[0]);
    
    return &rpp->base;
}