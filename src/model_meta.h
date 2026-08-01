/* SPDX-License-Identifier: MIT */
#ifndef HAX_MODEL_META_H
#define HAX_MODEL_META_H

#include "effort.h"

struct catalog_entry;
struct model_info;
struct provider;

/* Resolve metadata for a provider's selected model. Provider-reported values take precedence over
 * the models.dev catalog; context and image support may also be overridden by configuration.
 * Provider reports are scoped by model ID and copied into provider-owned storage. */

/* Cancel any active probe and release the provider's metadata storage. NULL-safe. Provider
 * destroy callbacks must call this before freeing the provider. */
void model_meta_release(struct provider *provider);

/* Clear stale metadata and asynchronously probe `model`. A stored report for the same model is
 * retained. Any previous probe is cancelled and joined first. NULL-safe. */
void model_meta_refresh(struct provider *provider, const char *model);

/* Wait for an active probe to finish without cancelling it. NULL-safe. */
void model_meta_wait(struct provider *provider);

/* Store a copy of provider-reported metadata, cancelling any active probe and replacing the
 * previous report. Reports without a model ID or any metadata fields are ignored. */
void model_meta_store(struct provider *provider, const struct model_info *info);

/* Copy the stored report into initialized `out`. Returns 1 when a report exists and 0 otherwise.
 * The caller must pass `out` to model_info_clear(). */
int model_meta_snapshot(const struct provider *provider, struct model_info *out);

/* Merge one provider report over one catalog entry. Either source may be NULL. `out` is a
 * metadata-only view with no owned fields and does not need clearing. */
void model_meta_merge(const struct model_info *reported, const struct catalog_entry *catalog,
                      struct model_info *out);

/* Context window in tokens, or 0 when unknown. The context_limit setting takes precedence. */
long model_meta_context(const struct provider *provider, const char *model);

/* Maximum output tokens per response, or 0 when unknown. */
long model_meta_max_output(const struct provider *provider, const char *model);

/* Resolve pricing fields and tiers into initialized `out`. Returns 1 when both input and output
 * rates are known. Other catalog fields remain unknown. */
int model_meta_rates(const struct provider *provider, const char *model, struct catalog_entry *out);

/* Return 1 when image input is supported, 0 when unsupported, and -1 when unknown. The image_input
 * setting takes precedence unless set to auto. */
int model_meta_image_input(const struct provider *provider, const char *model);

/* Resolve the categorical effort levels accepted by `model`, ordered by the provider's ladder.
 * `out` is always known; an empty set means the provider sends no categorical effort. */
void model_meta_efforts(const struct provider *provider, const char *model, struct effort_set *out);

#endif /* HAX_MODEL_META_H */
