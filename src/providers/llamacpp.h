/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_LLAMACPP_H
#define HAX_PROVIDERS_LLAMACPP_H

#include "provider.h"

/* Construct the llama-server preset. */
struct provider *llamacpp_provider_new(const char *name);

/* Reconcile a configured model against a /v1/models response. Returns 0 when the response names at
 * least one model and -1 otherwise. On success, `replacement` receives the first served model when
 * `configured_model` is empty or unavailable, or NULL when the configured model remains valid. The
 * caller frees a non-NULL replacement. */
int llamacpp_reconcile_model(const char *body, const char *configured_model, char **replacement);

/* Return an allocated warning that displays GGUF model paths as extensionless filenames. */
char *llamacpp_model_warning(const char *configured_model, const char *served_model);

/* Return an allocated display label. GGUF paths become extensionless filenames; other IDs are
 * copied unchanged. */
char *llamacpp_model_label(struct provider *provider, const char *model);

/* Return an allocated /props sibling URL, adding an encoded model query when nonempty. */
char *llamacpp_props_url(const char *base_url, const char *model);

extern const struct provider_factory PROVIDER_LLAMACPP;

#endif /* HAX_PROVIDERS_LLAMACPP_H */
