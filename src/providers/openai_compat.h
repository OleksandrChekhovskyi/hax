/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_COMPAT_H
#define HAX_PROVIDERS_OPENAI_COMPAT_H

#include "provider.h"

/* Construct a generic Chat Completions provider. openai.base_url is required; authentication uses
 * openai.api_key only and never falls back to OPENAI_API_KEY. */
struct provider *openai_compat_provider_new(const char *name);

extern const struct provider_factory PROVIDER_OPENAI_COMPAT;

#endif /* HAX_PROVIDERS_OPENAI_COMPAT_H */
