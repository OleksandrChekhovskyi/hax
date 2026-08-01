/* SPDX-License-Identifier: MIT */
#ifndef HAX_MODEL_META_H
#define HAX_MODEL_META_H

#include "effort.h"

struct provider;
struct model_info;
struct catalog_entry;

/*
 * Per-model metadata — context window, output cap, modalities, effort
 * levels — resolved from config, over what the backend said about this
 * exact model, over the models.dev snapshot, over the provider's static
 * defaults.
 *
 * The two middle tiers fail in opposite directions, which is why both
 * exist: a backend describes the model it will actually serve and knows
 * about one released this morning, but many describe nothing at all (real
 * OpenAI's /v1/models is bare ids) and one describes its models for its own
 * UI rather than for its API (see codex_parse_efforts). The snapshot has
 * broad coverage and lags.
 *
 * The live tier is filled by probe_model (provider.h) on every selection
 * change, and opportunistically by model_meta_remember when the /model
 * picker already holds a freshly fetched entry. The probe is what makes it
 * deterministic — figures that matter, like context and cost, must not
 * depend on whether the user happened to open a menu.
 *
 * Storage lives on struct provider so that two live providers keep separate
 * answers (see the `meta` field there). This module is stateless apart from
 * one mutex serializing the background writer against foreground reads.
 */

/* Cancel and join the in-flight probe, then free everything the provider's
 * metadata slot owns. Every provider's destroy() must call this before
 * releasing anything the worker could still be writing to. NULL-safe. */
void model_meta_release(struct provider *p);

/* Start fetching metadata for `model` (provider.h probe_model) and drop the
 * previous selection's snapshot. Settles any in-flight probe first, so a
 * superseded fetch can't land late over a newer answer. Returns
 * immediately; the accessors below start answering once it lands. A
 * snapshot that already describes `model` is kept as-is. */
void model_meta_refresh(struct provider *p, const char *model);

/* Wait for the in-flight probe, so the accessors below answer from the
 * live tier rather than from whatever had landed by chance. For a run that
 * issues its request immediately: the REPL gets this for free while the
 * user types their first prompt, but -p would otherwise resolve every
 * invocation from the catalog and the static defaults, and its one request
 * is the only chance to get the effort right. Bounded by the probe's own
 * timeout (struct model_probe). NULL-safe. */
void model_meta_settle(struct provider *p);

/* Adopt `m` as the live answer for the model it names — the picker handing
 * over an entry it just fetched, from the same document a probe would read.
 * Settles any in-flight probe rather than racing it.
 *
 * An entry stating no facts is ignored, in-flight probe included: a list
 * whose rows are bare ids (llama.cpp keeps its metadata in /props, not in
 * /v1/models) has nothing to hand over, and a snapshot naming the model is
 * exactly what makes model_meta_refresh skip the fetch. */
void model_meta_remember(struct provider *p, const struct model_info *m);

/* Copy the held snapshot out: 1 when *out was filled (caller owns it and
 * clears it with model_info_clear), 0 when nothing is held. Pairs with
 * model_meta_remember to put one back — the /model picker publishes a
 * candidate before the user has committed to it, and an abort owes the
 * running model the answers it already had. */
int model_meta_snapshot(const struct provider *p, struct model_info *out);

/* Context window in tokens, 0 when nothing knows: the context_limit config
 * override, then the live probe, then the catalog. */
long model_meta_context(const struct provider *p, const char *model);

/* Most output tokens one response may produce; 0 when unknown. */
long model_meta_max_output(const struct provider *p, const char *model);

/* Does the model accept image input? 1 yes, 0 no, -1 unknown. The
 * image_input config tristate pins the answer; otherwise live beats
 * catalog — llama.cpp vision depends on the mmproj loaded into this server
 * instance, which no snapshot can know. */
int model_meta_image_input(const struct provider *p, const char *model);

/* Rates to price one of `model`'s requests against, into *out — rates and
 * tiers only, the other fields left at their unknown sentinels. Returns 1
 * when input and output both resolved, i.e. catalog_price can price
 * against *out; 0 otherwise (*out is filled either way).
 *
 * Same live-over-catalog policy as the accessors above, and it decides
 * more here: a backend quotes what it will actually charge, margin
 * included, where the snapshot quotes the upstream's list price — and for
 * a provider with no catalog identity (openrouter) the live tier is the
 * only one there is.
 *
 * Price at *account* time and keep the result rather than asking again at
 * render time: the live tier follows the provider's current selection and
 * is dropped on the next model_meta_refresh, so a later call would re-rate
 * an old request against a model it never ran on. */
int model_meta_rates(const struct provider *p, const char *model, struct catalog_entry *out);

/* Reasoning-effort levels `model` accepts, into *out. Never returns
 * "unknown": an empty set means this model takes no categorical effort —
 * the provider sends none at all (llama.cpp), or the model's thinking is a
 * budget or a toggle — and callers skip the /effort step on that answer.
 *
 * Ordered by the provider's own ladder, with levels the ladder doesn't name
 * appended after it — but only when the backend reported them, since only
 * it speaks for the models it serves. The catalog narrows and never widens:
 * its id is shared, so it may be describing another API's vocabulary. The
 * append assumes an unrecognized level belongs at the expensive end, which
 * is what puts "max" last on backends whose static ladder predates it. */
void model_meta_efforts(const struct provider *p, const char *model, struct effort_set *out);

/* Layer one backend's report over one catalog entry, field by field —
 * the merge policy itself, with either side allowed to be NULL. The
 * accessors above run it over the stored snapshot; the /model picker runs
 * it per row over a whole enumerated catalog, where asking by model would
 * mean a lookup each. *out describes a model rather than being one: it
 * resolves the facts and carries neither id nor prose, so it owns nothing
 * and needs no freeing. */
void model_meta_merge(const struct model_info *reported, const struct catalog_entry *catalog,
                      struct model_info *out);

/* The level to use when `want` is not one `s` offers: the nearest at or
 * below it, else the lowest above it. Returns `want` when the set lists it,
 * NULL when nothing can be picked (empty set, or a name with no place in
 * the ladder). Borrows from `s`.
 *
 * Rounding down is deliberate: a level the model doesn't take must not
 * reach the wire (codex and Anthropic answer 400 rather than clamping), and
 * of the two neighbors the cheaper one is the safer place to land
 * silently. */
const char *effort_clamp(const struct effort_set *s, const char *want);

#endif /* HAX_MODEL_META_H */
