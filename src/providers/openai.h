/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_H
#define HAX_PROVIDERS_OPENAI_H

#include "provider.h"

/* Construct the api.openai.com provider; `id` becomes its stable identity. */
struct provider *openai_provider_new(const char *id);

extern const struct provider_factory PROVIDER_OPENAI;

#endif /* HAX_PROVIDERS_OPENAI_H */
