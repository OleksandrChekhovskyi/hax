/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_MOCK_H
#define HAX_PROVIDERS_MOCK_H

#include "provider.h"

/*
 * Mock provider for exercising the rendering / dispatch pipeline without
 * a real LLM. Activated with HAX_PROVIDER=mock.
 *
 * Two modes, picked at construction time:
 *
 *   1. Scripted — when HAX_MOCK_SCRIPT names a file, the mock plays one
 *      "turn" of directives per stream() call, in file order. See
 *      mock.c's header comment for the directive format.
 *
 *   2. Interactive — when HAX_MOCK_SCRIPT is unset, the mock parses the
 *      latest user message and emits a heuristic response: a backtick-
 *      quoted command becomes a bash (or read) tool call, anything else
 *      is echoed back. Lets you smoke-test the rendering with simple
 *      free-text instructions like "run `ls -la`".
 */

struct provider *mock_provider_new(void);

extern const struct provider_factory PROVIDER_MOCK;

#endif /* HAX_PROVIDERS_MOCK_H */
