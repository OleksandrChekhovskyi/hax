/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENAI_H
#define HAX_OPENAI_H

#include "provider.h"

/* Real OpenAI Chat Completions: locked to https://api.openai.com/v1. Reads:
 *   HAX_OPENAI_API_KEY   — preferred; falls back to OPENAI_API_KEY
 *   HAX_PROVIDER_NAME    — optional display name (defaults to "openai")
 *
 * Rejects HAX_OPENAI_BASE_URL — for custom OpenAI-compatible endpoints, use
 * the openai-compatible preset (openai_compat.h) instead, which keeps
 * OPENAI_API_KEY and prompt_cache_key scoped to the real-OpenAI host.
 *
 * Returns NULL on failure (prints cause to stderr). */
struct provider *openai_provider_new(void);

/* Wire-format dialect for reasoning parameters. OpenAI-compatible
 * backends agree on the rest of the Chat Completions payload but
 * diverge here — the divergence is structural (different field name
 * and shape, not just a value), so each backend declares which dialect
 * to emit. New shapes (DeepSeek's `thinking: {type}`, Qwen's
 * `enable_thinking`, …) get a new value here and a matching arm in
 * build_body().
 *
 *   REASONING_FLAT    `reasoning_effort: <effort>` as a top-level
 *                     string, sent only when effort is set. The
 *                     default — real OpenAI's Chat Completions
 *                     accepts exactly this shape and rejects others.
 *   REASONING_NESTED  `reasoning: {enabled: true, effort?: <effort>}`
 *                     as a top-level object, always sent when
 *                     selected. The `enabled: true` is the opt-in
 *                     some routers (OpenRouter, …) need to wake CoT
 *                     emission on models that otherwise stay silent. */
enum reasoning_format {
    REASONING_FLAT = 0,
    REASONING_NESTED,
};

/* Parse "flat" / "nested" into the matching enum value. `fallback` is
 * returned when `s` is NULL/empty so callers can write
 *   .reasoning_format = reasoning_format_parse(getenv("HAX_..."), REASONING_FLAT)
 * without an extra NULL check. An unrecognized non-empty value also
 * falls back, with a one-line stderr warning so a typo doesn't pass
 * silently. */
enum reasoning_format reasoning_format_parse(const char *s, enum reasoning_format fallback);

/* Preset configuration consumed by openai_provider_new_preset(). All fields
 * are optional except `default_base_url` (or HAX_OPENAI_BASE_URL must be
 * set). Used by the thin shim providers (openai-compatible, llama.cpp,
 * openrouter, …) that all speak the same Chat Completions translation but
 * want different defaults, auth fallbacks, and request headers. */
struct openai_preset {
    /* Display name when HAX_PROVIDER_NAME is unset. NULL → "openai". */
    const char *display_name;
    /* Default base URL when HAX_OPENAI_BASE_URL is unset. NULL is allowed
     * only when the preset requires HAX_OPENAI_BASE_URL (the constructor
     * fails if neither resolves to a non-empty URL). */
    const char *default_base_url;
    /* Optional second env var consulted for the API key, after
     * HAX_OPENAI_API_KEY. Each preset declares which global it picks up
     * (OPENAI_API_KEY for openai, OPENROUTER_API_KEY for openrouter, …);
     * unset for openai-compatible so a global OPENAI_API_KEY isn't
     * forwarded to an unrelated third-party endpoint. NULL → none. */
    const char *api_key_env;
    /* Whether to send prompt_cache_key by default. 0 = off (local servers,
     * generic compat backends), 1 = on (real OpenAI, OpenRouter, other
     * hosted compat backends with prefix caching).
     * HAX_OPENAI_SEND_CACHE_KEY=<anything> forces on. */
    int send_cache_key_default;
    /* Optional extra request headers, NULL-terminated array of
     * "Name: value" strings. Copied at construction; the preset does not
     * need to keep them alive afterwards. NULL → none. */
    const char *const *extra_headers;
    /* Wire format for reasoning parameters — see enum reasoning_format
     * above. Zero-initialized presets get REASONING_FLAT, the common
     * case for OpenAI Chat Completions and OpenAI-compatible backends. */
    enum reasoning_format reasoning_format;
    /* Ask the backend for mid-stream prefill progress. Sends
     * `return_progress: true` in the request body (a llama.cpp
     * extension) and tells the events parser to surface the resulting
     * top-level `prompt_progress` chunks as EV_PROGRESS. Other backends
     * ignore the unknown request field; leave this 0 unless the
     * preset's target server is known to emit prompt_progress. */
    int emit_progress;
};

/* Build an OpenAI-compatible provider configured by `preset`. NULL preset
 * is equivalent to a zero-initialized one (which will fail unless
 * HAX_OPENAI_BASE_URL is set). Returns NULL on failure. */
struct provider *openai_provider_new_preset(const struct openai_preset *preset);

/* Hand off ownership of a background probe to an openai-derived provider.
 * The handle is joined (with cancel first) by openai_destroy, which fits
 * preset shims like openrouter/llamacpp that spawn a context-window
 * probe but don't carry their own provider struct or destroy(). NULL
 * `probe` is a no-op (e.g. when probe_context_limit_spawn returned NULL
 * because pthread_create failed). Calling twice replaces the previous
 * handle without joining it — there's only one slot today; any provider
 * that needs to track several would carry its own struct instead of
 * piggybacking on this. */
struct bg_job;
void openai_attach_probe(struct provider *p, struct bg_job *probe);

extern const struct provider_factory PROVIDER_OPENAI;

#endif /* HAX_OPENAI_H */
