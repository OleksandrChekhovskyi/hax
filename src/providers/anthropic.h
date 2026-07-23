/* SPDX-License-Identifier: MIT */
#ifndef HAX_ANTHROPIC_H
#define HAX_ANTHROPIC_H

#include <jansson.h>

#include "provider.h"

/* Thinking control. Anthropic has two structurally different thinking modes
 * and hax has no model catalog to pick between them automatically, so the
 * mode is a preset default overridable per run (anthropic.thinking_mode):
 *
 *   ADAPTIVE  thinking:{type:"adaptive"} + a categorical output_config.effort.
 *             Claude decides when/how much to think; the only mode accepted by
 *             the current flagships (Opus 4.8/4.7 reject "enabled" with a 400),
 *             and the recommended one for Opus 4.6 / Sonnet 4.6. No beta header.
 *   BUDGET    thinking:{type:"enabled", budget_tokens:N}. The legacy mode for
 *             older Claude models, and what OpenAI-incompatible local servers
 *             that emulate the Messages API (llama-server) accept.
 *   OFF       omit the thinking parameter entirely.
 *
 * Real Anthropic defaults to ADAPTIVE; the anthropic-compatible shim defaults
 * to BUDGET. */
enum anthropic_thinking_mode {
    ANTHROPIC_THINKING_ADAPTIVE = 0,
    ANTHROPIC_THINKING_BUDGET,
    ANTHROPIC_THINKING_OFF,
};

/* Preset configuration consumed by anthropic_provider_new_preset(). Mirrors
 * the openai_preset split: a real-Anthropic shim and a bring-your-own-URL
 * compat shim share one Messages translation and differ only in defaults. */
struct anthropic_preset {
    /* Display name in the banner. NULL → "anthropic". */
    const char *display_name;
    /* Default base URL (the /v1 root) when anthropic.base_url is unset. NULL is
     * allowed only when the preset requires anthropic.base_url. */
    const char *default_base_url;
    /* Second env var consulted for the API key after anthropic.api_key (e.g.
     * ANTHROPIC_API_KEY for the real preset). NULL → none, so a global key
     * doesn't leak to an arbitrary compat endpoint. */
    const char *api_key_env;
    /* Ignore the shared anthropic.base_url override (the real preset, whose
     * endpoint is fixed at api.anthropic.com). */
    int lock_base_url;
    /* Thinking mode default (overridable via anthropic.thinking_mode). */
    enum anthropic_thinking_mode default_thinking_mode;
    /* Round-trip a thinking block whose signature is empty as-is, rather than
     * downgrading it to a plain text block. Compat backends (llama-server)
     * emit and accept empty signatures; real Anthropic requires a valid one,
     * so the real preset leaves this 0 and empty-sig blocks degrade to text. */
    int allow_empty_signature;
    /* Emit prompt-caching cache_control breakpoints by default (real
     * Anthropic on; compat off, since a server that rejects the field would
     * 400). Overridable via anthropic.cache. */
    int send_cache_control_default;
    /* Config namespace for the settings this provider resolves itself
     * (base_url, api_key, version, max_tokens, thinking_mode, thinking_budget,
     * cache, cache_ttl). NULL — the compiled-in shims — reads the shared global
     * "anthropic.*" keys (env-bound via HAX_ANTHROPIC_*) and the global
     * provider_name for the banner. A config-defined provider sets this to
     * "providers.<name>" so each named provider reads its own self-contained
     * subtree and a stray HAX_ANTHROPIC_* can't bleed in. */
    const char *config_prefix;
    /* Model-catalog identity, copied to provider->catalog_id (see
     * provider.h). Borrowed — a static literal or a config-tier string
     * outliving the provider. NULL = no catalog presence. */
    const char *catalog_id;
};

/* Build an Anthropic Messages provider configured by `preset`. Returns NULL on
 * failure (prints cause to stderr). */
struct provider *anthropic_provider_new_preset(const struct anthropic_preset *preset);

/* Real Anthropic: locked to https://api.anthropic.com/v1, ANTHROPIC_API_KEY,
 * adaptive thinking. `name` is the factory's own name (unused). */
struct provider *anthropic_provider_new(const char *name);

/* The Anthropic adaptive-thinking effort ladder, low→high:
 * low, medium, high, xhigh, max. Passed verbatim as output_config.effort;
 * per-model support is gated server-side (xhigh/max are rejected on models
 * that don't support them), so the picker offers the full ladder and lets the
 * API narrow it. */
extern const char *const ANTHROPIC_EFFORT_LADDER[];
extern const size_t ANTHROPIC_EFFORT_LADDER_N;

/* Translate flat conversation items into the Messages `messages` array
 * (content-block form). Exposed for unit testing the round-trip — notably the
 * thinking-block replay and tool_result coalescing — without an HTTP call.
 * A reasoning item's stored block (reasoning_json) is replayed only when its
 * provenance stamp matches cur_provider/cur_model. When `allow_empty_signature`
 * is 0, a thinking block with an empty signature is downgraded to a text block
 * (or dropped if it has no text). `image_input` is the context flag (1/-1
 * serialize tool-result image parts as image blocks, 0 as text placeholders).
 * Returns a new jansson array; caller frees. */
json_t *anthropic_build_messages(const struct item *items, size_t n, const char *cur_provider,
                                 const char *cur_model, int allow_empty_signature, int image_input);

extern const struct provider_factory PROVIDER_ANTHROPIC;

#endif /* HAX_ANTHROPIC_H */
