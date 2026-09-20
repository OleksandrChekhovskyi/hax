/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_H
#define HAX_PROVIDERS_VERTEX_H

struct provider_availability; /* provider.h */
struct provider_def;          /* providers/registry.h */

/* Endpoint construction and picker presentation for the vertex def. Google credential handling,
 * renewal, and recovery live in vertex_auth.c and are not part of this boundary. */

/* Resolve the endpoint's base URL (scheme + host) from the validated project and location: the
 * `global` endpoint, the us/eu multi-region endpoints, or the per-location regional host. Returns
 * an owned URL, or NULL after reporting the invalid or missing value. */
char *vertex_resolve_base_url(const struct provider_def *def);

/* Resolve the raw-predict path from the same project/location precedence as the endpoint host. */
char *vertex_resolve_path(const struct provider_def *def);

/* Immediate picker verdict with a concise reason: project, credential source, and gcloud when
 * needed. Checks local files only; no network probe or process execution. */
void vertex_prepare_availability(const struct provider_def *def, struct provider_availability *out);

#endif /* HAX_PROVIDERS_VERTEX_H */