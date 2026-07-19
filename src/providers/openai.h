/* SPDX-License-Identifier: MIT */
#ifndef HAX_OPENAI_H
#define HAX_OPENAI_H

#include <jansson.h>

#include "provider.h"

/* Real OpenAI Chat Completions: locked to https://api.openai.com/v1. Reads:
 *   HAX_OPENAI_API_KEY   — preferred; falls back to OPENAI_API_KEY
 *   HAX_PROVIDER_NAME    — optional display name (defaults to "openai")
 *
 * Rejects HAX_OPENAI_BASE_URL — for custom OpenAI-compatible endpoints, use
 * the openai-compatible preset (openai_compat.h) instead, which keeps
 * OPENAI_API_KEY and prompt_cache_key scoped to the real-OpenAI host.
 *
 * `name` is the provider_factory name, unused here (the real-OpenAI factory
 * serves exactly one provider). Returns NULL on failure (prints cause to
 * stderr). */
struct provider *openai_provider_new(const char *name);

/* Reasoning-parameter wire format. Add a value and build_body() arm for new
 * shapes. Both forms are omitted when effort is unset.
 *   REASONING_FLAT:   `reasoning_effort: <effort>` (the OpenAI default)
 *   REASONING_NESTED: `reasoning: {enabled: true, effort: <effort>}`; "none"
 *                     sends `{enabled: false}` to disable. */
enum reasoning_format {
    REASONING_FLAT = 0,
    REASONING_NESTED,
};

/* Parse "flat" / "nested" into the matching enum value. `fallback` is
 * returned when `s` is NULL/empty so callers can write
 *   .reasoning_format = reasoning_format_parse(config_str("openai...."), REASONING_FLAT)
 * without an extra NULL check. An unrecognized non-empty value also
 * falls back, with a one-line stderr warning so a typo doesn't pass
 * silently. */
enum reasoning_format reasoning_format_parse(const char *s, enum reasoning_format fallback);

/* Encode the reasoning-effort request field into `body` for `fmt` (see enum
 * reasoning_format for the wire shapes). A NULL/empty effort omits the field
 * entirely, leaving the provider's own default; otherwise the level is
 * requested, with "none" disabling. Exposed so the encoding can be unit-tested
 * without an HTTP round-trip. */
void openai_apply_reasoning(json_t *body, enum reasoning_format fmt, const char *effort);

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
    /* Ask the backend for usage accounting. Sends `usage: {include: true}`
     * in the request body (an OpenRouter extension) so the trailing usage
     * chunk carries the response's `cost` in USD. Gated per preset: the
     * field name is generic enough that an unknown-field-rejecting backend
     * (vLLM) could 400 on it. */
    int request_cost;
    /* When non-NULL, captured reasoning text (ITEM_REASONING.reasoning_text)
     * is round-tripped back to the server under this field name on each
     * assistant message — "reasoning_content" for llama.cpp. Required for
     * interleaved-thinking models (Qwen3): without their prior reasoning in
     * the prompt they degrade and leak tool calls into the reasoning
     * channel. NULL = don't send (real OpenAI exposes no such field).
     * HAX_REASONING_ROUNDTRIP overrides this per-run (off / on / <field>). */
    const char *roundtrip_reasoning_field;
    /* Reasoning-effort wire values this backend accepts, surfaced by the
     * provider's list_efforts for the /effort picker. Borrowed (typically
     * OPENAI_EFFORT_LADDER, a static array) — not copied, so it must outlive
     * the provider. NULL / 0 means "no categorical effort" (local servers
     * whose thinking is a token budget): the picker then skips the step. */
    const char *const *efforts;
    size_t n_efforts;
    /* Appended to the "response incomplete: length" error when a stream is
     * truncated with finish_reason "length", to make a backend-specific
     * cause actionable. The canonical case is a local server whose context
     * window is too small for the prompt (ollama's num_ctx default, a small
     * llama.cpp -c): the generic "length" tells the user nothing they can
     * act on. Borrowed static string; NULL = no hint (hosted backends, where
     * "length" means the model's own max-output cap). */
    const char *length_hint;
    /* The base URL is fixed to default_base_url: ignore the shared
     * HAX_OPENAI_BASE_URL override (which belongs to the bring-your-own-URL
     * presets — openai-compatible, llama.cpp, ollama). Set by openai and
     * openrouter, whose endpoints aren't configurable, so a base URL left set
     * for another backend can't redirect them (or block their selection). */
    int lock_base_url;
    /* Config namespace for the settings this constructor resolves itself
     * (base_url, api_key, send_cache_key, reasoning_roundtrip). NULL — the
     * compiled-in shims — reads the shared global "openai.*" keys (and the
     * global provider_name for the banner): the env/ad-hoc single-provider
     * lane. A config-defined provider sets this to "providers.<name>", so each
     * named provider reads its own self-contained subtree and a stray
     * HAX_OPENAI_* in the environment can't bleed into it. */
    const char *config_prefix;
    /* Model-catalog identity, copied to provider->catalog_id (see
     * provider.h). Borrowed — a static literal or a config-tier string
     * outliving the provider. NULL = no catalog presence. */
    const char *catalog_id;
};

/* The shared "OpenAI-style" reasoning-effort ladder, low→high:
 * none, minimal, low, medium, high, xhigh. OpenAI, OpenRouter, and the
 * generic compat preset all accept this vocabulary (OpenRouter maps an
 * unsupported level to the nearest one; real OpenAI may reject a level a
 * given model doesn't support). Presets point .efforts at this. */
extern const char *const OPENAI_EFFORT_LADDER[];
extern const size_t OPENAI_EFFORT_LADDER_N;

/* Build an OpenAI-compatible provider configured by `preset`. NULL preset
 * is equivalent to a zero-initialized one (which will fail unless
 * HAX_OPENAI_BASE_URL is set). Returns NULL on failure. */
struct provider *openai_provider_new_preset(const struct openai_preset *preset);

/* Shared provider-picker availability helpers (provider_factory.available),
 * so each openai-family shim's check is a one-liner. Both set *reason (a
 * static string) and return 0 when unavailable, 1 when usable.
 *
 *   openai_key_available    — usable iff HAX_OPENAI_API_KEY or `api_key_env`
 *     (when non-NULL) holds a non-empty key. For hosted backends.
 *     `miss_reason` is the caller's exact no-key message (a static string
 *     naming its env var, e.g. "OPENAI_API_KEY not set"), so the picker's
 *     reason is actionable rather than a generic "no API key".
 *   openai_base_url_reachable — bounded GET <base_url>/models with a short
 *     timeout; usable iff it 2xx's. For local-server shims (llama.cpp,
 *     ollama) whose availability is "is the server up". May block briefly,
 *     so the picker runs it on a worker thread. */
int openai_key_available(const char *api_key_env, const char *miss_reason, const char **reason);
int openai_base_url_reachable(const char *base_url, const char *api_key, const char **reason);

/* Hand off ownership of a background probe to an openai-derived provider.
 * The handle is joined (with cancel first) by openai_destroy, which fits
 * preset shims like openrouter/llamacpp that spawn a context-window
 * probe but don't carry their own provider struct or destroy(). NULL
 * `probe` is a no-op (e.g. when probe_context_limit_spawn returned NULL
 * because pthread_create failed). There's one slot; before re-attaching
 * (a /model re-probe) the caller settles the old handle via
 * openai_context_probe_reset, so attach itself stays a plain setter. A
 * provider needing several probes would carry its own struct instead of
 * piggybacking on this. */
struct bg_job;
void openai_attach_probe(struct provider *p, struct bg_job *probe);

/* Cancel and join any in-flight context probe and reset context_limit to
 * unknown (0), readying an openai-derived provider for a fresh probe after
 * a runtime /model switch. Pairs with a follow-up openai_attach_probe.
 * Used by the model-keyed shims (openrouter) in their refresh_context. */
void openai_context_probe_reset(struct provider *p);

/* Translate flat conversation items into the Chat Completions `messages`
 * array. Exposed (rather than static) so the round-trip serialization —
 * notably reasoning_content attachment — can be unit-tested without an HTTP
 * round-trip. `reasoning_field` NULL means don't emit reasoning. When it is
 * set, a reasoning item's text is replayed only if its provenance stamp
 * matches `cur_provider`/`cur_model` (both must be non-NULL and equal), so a
 * mid-conversation provider or model switch never feeds stale CoT to the new
 * backend. Returns a new jansson array the caller must json_decref. */
json_t *openai_build_messages(const char *system_prompt, const struct item *items, size_t n,
                              const char *reasoning_field, const char *cur_provider,
                              const char *cur_model);

extern const struct provider_factory PROVIDER_OPENAI;

#endif /* HAX_OPENAI_H */
