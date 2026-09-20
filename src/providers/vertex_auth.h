/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_AUTH_H
#define HAX_PROVIDERS_VERTEX_AUTH_H

#include "providers/http_provider.h"

struct provider_def; /* providers/registry.h */

/* Google Application Default Credentials for the vertex provider: an explicit access token, an
 * authorized_user ADC file refreshed natively, or any other ADC kind delegated to gcloud.
 * Credential loading, renewal, recovery, and the http_auth_ops live here; endpoint construction
 * and picker presentation stay in vertex.c. No network or process execution ever runs on the
 * synchronous credential-status path. */

/* Auth-source hook for the vertex def: open a Google-credential session as `out`'s state. A
 * missing or broken credential source does not fail construction — the session reports setup
 * steps on the first request. The session re-reads the ADC file and refreshes short-lived
 * access tokens across the session's lifetime. */
int vertex_auth_source(const struct provider_def *def, struct http_auth_source *out);

/* Classify local credential setup for the picker. Returns 1 when a usable source exists; on 0,
 * `reason` receives an owned short reason ("ADC unavailable" or "gcloud not found"). Checks local
 * files and PATH only; never performs network I/O or runs gcloud. */
int vertex_auth_local_status(char **reason);

#endif /* HAX_PROVIDERS_VERTEX_AUTH_H */