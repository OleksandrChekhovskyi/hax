/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_H
#define HAX_PROVIDERS_VERTEX_H

#include "provider.h"
#include "providers/http_provider.h"

struct provider_def;

/* Construct a Vertex AI provider (OpenAI-compatible endpoint) on top of the
 * generic HTTP provider. */
struct provider *vertex_provider_new(const struct provider_def *def);

/* Availability check (project + credentials). */
void vertex_prepare_availability(const struct provider_def *def,
                                struct provider_availability *out);

/* Auth source for Vertex AI (static token or application default credentials). */
int vertex_auth_source(const struct provider_def *def, struct http_auth_source *out);

/* Rewrite a bare Gemini model id to its publisher-prefixed form. */
char *vertex_rewrite_model(const char *model);

#endif
