/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_LLAMACPP_H
#define HAX_PROVIDERS_LLAMACPP_H

#include <jansson.h>

#include "provider.h"

struct provider_def; /* providers/registry.h */

/* Hooks behind the llamacpp def: a local llama-server whose endpoint derives from a configured
 * port and whose active model is discovered from the running server. */

/* Discovery hook: reconcile the configured model against the running server, adopting or
 * correcting it as a non-persisted override. Fails (reported) when the server is unreachable
 * and no model is configured to fall back on. */
int llamacpp_discover(const char *base_url, int *model_discovered);

/* Availability probe against the resolved local base URL. */
void llamacpp_prepare_availability(const struct provider_def *def,
                                   struct provider_availability *out);

/* Single-model metadata from llama-server's /props endpoint. */
int llamacpp_probe_model(struct provider *provider, const char *model, struct model_probe *probe);

/* Decision derived from a /v1/models response. A classic single-model server reports what it
 * serves, so an unavailable configured model is substituted. A router catalog (entries carrying a
 * status object) lists every known model rather than what is running, so an absent configured
 * model is dropped instead, and an unconfigured one is filled in only when exactly one model is
 * running. */
struct llamacpp_reconcile {
    char *replacement;    /* owned; model to use instead of the configured one */
    char *canonical;      /* owned; catalog id of a configured alias */
    int clear_configured; /* configured model is absent from a router catalog */
    int no_models;        /* valid response listing no models */
};

/* Reconcile a configured model against a /v1/models response. Returns 0 and fills `decision` when
 * the response is usable and -1 otherwise. Models match by id or alias. The caller frees
 * `decision->replacement` and `decision->canonical`. */
int llamacpp_reconcile_model(const char *body, const char *configured_model,
                             struct llamacpp_reconcile *decision);

/* Populate picker metadata from a /v1/models entry: context from meta.n_ctx, image support from
 * architecture.input_modalities, and any router status other than "unloaded" as the description. */
void llamacpp_parse_model(const json_t *entry, struct model_info *info);

/* Return an allocated warning that displays GGUF model paths as extensionless filenames. */
char *llamacpp_model_warning(const char *configured_model, const char *served_model);

/* Return an allocated display label. GGUF paths become extensionless filenames; other IDs are
 * copied unchanged. */
char *llamacpp_model_label(struct provider *provider, const char *model);

/* Return an allocated /props sibling URL, adding an encoded model query when nonempty. */
char *llamacpp_props_url(const char *base_url, const char *model);

#endif /* HAX_PROVIDERS_LLAMACPP_H */
