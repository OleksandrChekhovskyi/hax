/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_H
#define HAX_PROVIDERS_VERTEX_H

#include <stddef.h>

#include "provider.h"
#include "providers/http_provider.h"

struct provider_def;
struct wire;

/* Construct a Vertex AI provider (OpenAI-compatible endpoint) on top of the
 * generic HTTP provider. */
struct provider *vertex_provider_new(const struct provider_def *def);

/* Availability check (project + credentials). */
void vertex_prepare_availability(const struct provider_def *def, struct provider_availability *out);

/* Auth source for Vertex AI (static token or application default credentials). */
int vertex_auth_source(const struct provider_def *def, struct http_auth_source *out);

/* Rewrite a bare Gemini model id to its publisher-prefixed form. A Claude id keeps its own
 * shape: its endpoint names the model in the path. */
char *vertex_rewrite_model(const char *model);

/* A publisher of the Vertex catalog, and the shape of the model ids it uses. */
struct vertex_publisher {
    const char *publisher;   /* the publisher segment of the resource name */
    const char *chat_prefix; /* what a chat model id of this publisher starts with */
    /* The id carries the version, as the Claude endpoint names its models. */
    int versioned;
};

/* The publishers whose chat models the catalog lists. */
extern const struct vertex_publisher VERTEX_PUBLISHERS[];
extern const size_t VERTEX_N_PUBLISHERS;

/* The publisher of a model id, or NULL when no publisher claims it. */
const struct vertex_publisher *vertex_publisher_for_id(const char *model);

/* Whether the model is served by the Anthropic endpoint rather than the OpenAI-compatible one. */
int vertex_model_is_anthropic(const char *model);

/* The wire a model speaks, or NULL to keep the provider's default. */
const struct wire *vertex_wire_for_model(const struct provider *provider, const char *model);

/* The request's base URL for a model whose endpoint names it in the path, or NULL to use the
 * provider's base URL. */
char *vertex_request_base(const struct provider *provider, const char *model);

/* The scheme, host, and port of the configured Vertex endpoint. The native publisher API lives
 * on it, not on the OpenAI-compatible path. */
char *vertex_api_origin(void);

/* The project the calls belong to. */
char *vertex_project_id(void);

/* /usage backend: Cloud Monitoring quota usage for the project. */
int vertex_query_usage(struct provider *provider);

#endif /* HAX_PROVIDERS_VERTEX_H */
