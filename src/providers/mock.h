/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_MOCK_H
#define HAX_PROVIDERS_MOCK_H

#include "provider.h"

/* Construct the mock provider; providers.mock.script is captured at
 * construction. */
struct provider *mock_provider_new(const char *id);

extern const struct provider_factory PROVIDER_MOCK;

#endif /* HAX_PROVIDERS_MOCK_H */
