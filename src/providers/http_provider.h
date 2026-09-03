/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_HTTP_PROVIDER_H
#define HAX_PROVIDERS_HTTP_PROVIDER_H

#include <jansson.h>

#include "provider.h"

struct provider_def; /* providers/registry.h */

/* Generic streaming-HTTP provider: endpoint, credential, and configuration resolution, request
 * policy, and stream mechanics for any wire dialect. Every provider is built from its
 * provider_def; hook modules implementing their own metadata protocol use the accessors below. */

/* Per-entry /models refinement: fill capabilities and pricing from one listing entry. `out` is
 * initialized and already owns the entry's id. */
typedef void (*http_parse_model_cb)(const json_t *entry, struct model_info *out);

/* Ops of a dynamic credential source, for endpoints whose tokens rotate rather than sit in a
 * config key. Every op receives the source's own `auth` state, and destroy releases it. All
 * ops run on the foreground thread. */
struct http_auth_ops {
    /* Ensure usable credentials before an operation. With `allow_refresh` the source may renew
     * them over the network, honoring `tick`; without it only local reloads are permitted, so
     * callers that must not block simply fail on a stale token. Returns 0 when credentials
     * exist, -1 when the provider is logged out. */
    int (*prepare)(void *auth, int allow_refresh, http_tick_cb tick, void *tick_user);
    /* Owned NULL-terminated credential headers, rebuilt per attempt because tokens rotate.
     * `session_id` is the request's affinity id — the conversation's, or the provider's
     * per-process fallback outside one; `streaming` distinguishes the streaming completion
     * request from metadata GETs. */
    char **(*headers)(const void *auth, const char *session_id, int streaming);
    /* One bounded recovery after an unauthorized response: renew or re-read credentials and
     * return non-zero to retry with rebuilt headers. `retried` is the caller's per-operation
     * guard; the op must set it. */
    int (*recover)(void *auth, int *retried, http_tick_cb tick, void *tick_user);
    /* Owned user-facing message for a terminal unauthorized failure, logged out or rejected.
     * May record state so the next operation re-evaluates credentials. */
    char *(*unauthorized_message)(void *auth);
    void (*destroy)(void *auth);
};

/* A credential source: its ops and their state. A provider with a source resolves no API key —
 * its requests authenticate through the source's headers. A zeroed value means no source. */
struct http_auth_source {
    const struct http_auth_ops *ops;
    void *state; /* owned by the source; released by ops->destroy */
};

/* Build the provider described by `def`: its data overlaid by the providers.<id> config block
 * (registry.h), its capability hooks installed. NULL after reporting a user-actionable error;
 * callers normally go through provider_construct instead. */
struct provider *http_provider_new(const struct provider_def *def);

/* Availability for the /provider picker, from the same def and config resolution as
 * construction. A keyed (cloud) def — one with a declared api_key_env or an inline api_key —
 * is selectable iff that key resolves, with no network probe (fast, and a 401 would be the
 * only extra signal). A keyless one counts its resolved base_url as availability, except for
 * defs that opt into a GET <base>/models reachability probe. `out` need not be initialized. */
void http_provider_availability(const struct provider_def *def, struct provider_availability *out);

/* Accessors for hook modules implementing their own metadata callbacks. */
const char *http_provider_base_url(const struct provider *provider);
int http_provider_has_api_key(const struct provider *provider);
/* The resolved key, or NULL; borrowed for the provider's lifetime. */
const char *http_provider_api_key(const struct provider *provider);
/* Owned NULL-terminated auth headers for JSON metadata requests, following the resolved
 * metadata dialect (x-api-key plus the version header on the Anthropic side); free with
 * string_array_free. */
char **http_provider_metadata_headers(const struct provider *provider);
/* Owned NULL-terminated extra headers, def defaults under the user's, expanded for a request
 * outside any conversation; NULL when none. Free with string_array_free. */
char **http_provider_extra_headers(const struct provider *provider);
/* The def's per-entry /models refinement hook, or NULL. */
http_parse_model_cb http_provider_parse_model(const struct provider *provider);

/* The provider's credential source; its ops member is NULL when the provider authenticates
 * with an API key instead. */
const struct http_auth_source *http_provider_auth(const struct provider *provider);
/* Note that a model-metadata probe could not run under stale credentials; the next stream
 * that authenticates successfully relaunches it. */
void http_provider_defer_probe(struct provider *provider);

/* Output cap for one Messages request: <prefix>.max_tokens clamped to model metadata. */
int http_provider_max_tokens(struct provider *provider, const char *model);

/* Populate an owned GET <base_url>/models availability request. `extra_headers` (may be NULL)
 * are copied after the Authorization header. */
void http_provider_prepare_base_url_availability(const char *base_url, const char *api_key,
                                                 char *const *extra_headers,
                                                 struct provider_availability *out);

#endif /* HAX_PROVIDERS_HTTP_PROVIDER_H */
