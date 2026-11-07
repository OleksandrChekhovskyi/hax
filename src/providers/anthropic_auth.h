/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_AUTH_H
#define HAX_PROVIDERS_ANTHROPIC_AUTH_H

struct provider;
struct provider_def;
struct provider_availability;
struct http_auth_source;

int anthropic_auth_source(const struct provider_def *def, struct http_auth_source *out);
void anthropic_provider_reload_auth(struct provider *provider);
void anthropic_prepare_availability(const struct provider_def *def,
                                    struct provider_availability *out);

#endif /* HAX_PROVIDERS_ANTHROPIC_AUTH_H */
