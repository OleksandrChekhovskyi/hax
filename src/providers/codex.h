/* SPDX-License-Identifier: MIT */
#ifndef HAX_CODEX_H
#define HAX_CODEX_H

#include "provider.h"

/* Reads ~/.codex/auth.json (tokens.access_token + tokens.account_id).
 * Returns NULL on failure (prints cause to stderr). */
struct provider *codex_provider_new(void);

extern const struct provider_factory PROVIDER_CODEX;

#endif /* HAX_CODEX_H */
