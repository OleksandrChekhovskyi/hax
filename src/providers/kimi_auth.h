/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_KIMI_AUTH_H
#define HAX_PROVIDERS_KIMI_AUTH_H

#include <jansson.h>

struct provider;
struct provider_def;
struct provider_availability;
struct http_auth_source;
struct model_info;

int kimi_auth_source(const struct provider_def *def, struct http_auth_source *out);
void kimi_provider_reload_auth(struct provider *provider);
void kimi_prepare_availability(const struct provider_def *def, struct provider_availability *out);
void kimi_parse_model(const json_t *entry, struct model_info *out);

#endif /* HAX_PROVIDERS_KIMI_AUTH_H */
