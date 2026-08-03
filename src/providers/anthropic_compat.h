/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_COMPAT_H
#define HAX_PROVIDERS_ANTHROPIC_COMPAT_H

#include "provider.h"

/* Construct a generic Messages API provider. anthropic.base_url is required; authentication uses
 * anthropic.api_key only and never falls back to ANTHROPIC_API_KEY. */
struct provider *anthropic_compat_provider_new(const char *name);

extern const struct provider_factory PROVIDER_ANTHROPIC_COMPAT;

#endif /* HAX_PROVIDERS_ANTHROPIC_COMPAT_H */
