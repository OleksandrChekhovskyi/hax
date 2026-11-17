/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDER_ROUTER_H
#define HAX_PROVIDER_ROUTER_H

#include <stddef.h>

#include "agent.h"
#include "provider.h"
#include "transport/http.h"

/* Build a router from a NULL-terminated array of provider ids (config order).
 * Returns NULL on allocation failure. */
struct provider_router *provider_router_new(const char *const *provider_ids, size_t count);

/* Stream implementation that tries each provider in order on terminal errors. */
int provider_router_stream(struct provider *router_provider, const struct context *context,
                           const char *model, stream_cb callback, void *user, http_tick_cb tick,
                           void *tick_user);

/* Destroy and free all resources. */
void provider_router_destroy(struct provider_router *router);

/* Get the currently active provider (for model resolution, metadata, etc.). */
struct provider *provider_router_active(const struct provider_router *router);

/* Check if router is active (vs single provider). */
int provider_is_router(const struct provider *provider);

/* Get the current (1-based) and total provider counts. Returns 1 if active, 0 otherwise. */
int provider_router_position(const struct provider *provider, size_t *out_current,
                             size_t *out_total);

/* Exported helper to build router from config. Returns NULL when fallback is not
 * usable (no providers configured, allocation failure, etc.). */
struct provider *provider_router_construct_from_config(void);

#endif /* HAX_PROVIDER_ROUTER_H */