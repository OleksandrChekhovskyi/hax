/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_H
#define HAX_PROVIDERS_VERTEX_H

#include "provider.h"

/* Construct the Google Vertex AI provider. It serves the Anthropic Messages protocol on a
 * per-model "...:streamRawPredict" endpoint, authenticated with a Google OAuth2 Bearer token
 * (see vertex_auth.h). vertex.base_url / HAX_VERTEX_BASE_URL must name the complete stream
 * endpoint; model discovery is not possible, so set one with HAX_MODEL or /model. */
struct provider *vertex_provider_new(const char *name);

extern const struct provider_factory PROVIDER_VERTEX;

#endif /* HAX_PROVIDERS_VERTEX_H */
