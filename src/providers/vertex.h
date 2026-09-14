/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_H
#define HAX_PROVIDERS_VERTEX_H

struct provider_availability; /* provider.h */
struct provider_def;          /* providers/registry.h */
struct http_auth_source;      /* providers/http_provider.h */

/* Auth-source hook for the vertex def: open a Google-credential session as `out`'s state. A
 * missing or broken credential source does not fail construction — the session reports the
 * resolution steps on the first request and on availability. The session re-reads the ADC file
 * and refreshes short-lived access tokens across the session's lifetime. */
int vertex_auth_source(const struct provider_def *def, struct http_auth_source *out);

/* Resolve the endpoint's base URL (scheme + host) from the resolved project and location: the
 * `global` endpoint, the us/eu multi-region endpoints, or the per-location regional host. Returns
 * an owned URL, or NULL after reporting which value is missing. */
char *vertex_resolve_base_url(const struct provider_def *def);

/* Immediate availability verdict for the picker: project/location plus usable credentials, with
 * no network probe. */
void vertex_prepare_availability(const struct provider_def *def, struct provider_availability *out);

#endif /* HAX_PROVIDERS_VERTEX_H */
