/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDER_H
#define HAX_PROVIDER_H

#include <stdatomic.h>
#include <stddef.h>

#include "transport/http.h"

/*
 * A flat, provider-agnostic view of a conversation. Each item is one thing:
 * a user message, an assistant message, a tool call, or a tool result.
 * This maps cleanly to the OpenAI Responses API "input" array, and with a
 * small amount of grouping in the adapter, to Anthropic Messages too.
 */
enum item_kind {
    ITEM_USER_MESSAGE,
    ITEM_ASSISTANT_MESSAGE,
    ITEM_TOOL_CALL,
    ITEM_TOOL_RESULT,
    /* Reasoning carried across turns. Two flavors share this kind and the
     * reasoning_* fields on struct item: the Codex Responses adapter emits
     * an opaque encrypted blob (reasoning_json) for chain-of-thought
     * continuity, while the openai-family adapters capture plain CoT text
     * (reasoning_text) to replay as reasoning_content for interleaved-
     * thinking models. Adapters that need neither simply skip this kind. */
    ITEM_REASONING,
    /* Inert marker the agent emits before each fresh model request, so
     * downstream consumers (currently the transcript renderer) can mark
     * turn boundaries the same way agent.c/turn.c sees them — one per
     * HTTP round-trip — without re-deriving them by heuristics. Carries
     * no fields. Provider adapters ignore it when serializing the
     * conversation. */
    ITEM_TURN_BOUNDARY,
    /* Inert accounting footer for the model round-trip whose items
     * precede it, appended by the agent when the round-trip completes
     * (its usage is only known then, which is why it trails the turn
     * rather than riding the boundary that opened it). Carries a
     * `usage` payload; the transcript renders it as a per-request stats
     * line. Provider adapters ignore it like ITEM_TURN_BOUNDARY. */
    ITEM_TURN_USAGE,
};

/* One inline image carried by an item: base64 payload plus the metadata
 * adapters, display, and persistence need. Dimensions are parsed from the
 * image header at ingestion (0 = unknown). */
struct item_image {
    char *mime;     /* "image/png", "image/jpeg", "image/gif", "image/webp" */
    char *data_b64; /* owned base64 payload */
    long width;
    long height;
};

struct item {
    enum item_kind kind;
    /* USER_MESSAGE / ASSISTANT_MESSAGE: */
    char *text;
    /* TOOL_CALL / TOOL_RESULT: */
    char *call_id;
    /* TOOL_CALL: */
    char *tool_name;
    char *tool_arguments_json;
    /* TOOL_RESULT: */
    char *output;
    /* TOOL_RESULT: images attached alongside `output` (owned array of
     * owned members; freed by item_free). Adapters serialize them as
     * native image blocks or text placeholders depending on
     * context.image_input (see below). NULL/0 everywhere else. */
    struct item_image *images;
    size_t n_images;
    /* REASONING round-trip form (Codex): full JSON object (e.g.
     * {"type":"reasoning","id":...,"summary":[...],"encrypted_content":...})
     * ready to be re-sent. This is what the model needs back. */
    char *reasoning_json;
    /* REASONING human-readable form: plain chain-of-thought text. For the
     * openai-family it arrives via `reasoning_content`/`reasoning` and is
     * round-tripped as reasoning_content when the provider opts in (see
     * openai_preset.roundtrip_reasoning_field). For Codex it is the streamed
     * summary, carried alongside reasoning_json purely for display (the JSON
     * is what gets re-sent). An item may have either or both; the transcript
     * prefers this text and falls back to the opaque reasoning_json tag. */
    char *reasoning_text;
    /* REASONING provenance: the provider name and model id that produced this
     * item, stamped when it enters history (and round-tripped through the
     * session log). reasoning_json is signed/bound to that exact model, so the
     * build path replays it only when this stamp matches the provider+model of
     * the current request — after a /model or /provider switch (mid-session or
     * across a resume) the older items carry the old stamp and are skipped.
     * NULL on non-reasoning items and on records that predate stamping. */
    char *provider;
    char *model;
    /* USER_MESSAGE: this is a compaction seed — the synthetic summary that
     * replaced compacted history — not something the user typed. Provider
     * adapters ignore it (the seed goes on the wire as a plain user
     * message); the flag exists so display and session tooling (resume
     * replay, the /resume picker label) never present it as a typed
     * prompt. Round-tripped through the session log. */
    int compact_seed;
    /* TURN_USAGE: malloc'd accounting payload (owned; freed by
     * item_free). NULL on every other kind. */
    struct turn_usage *usage;
};

void item_free(struct item *it);

/* One-line text stand-in for an image part — "[image: image/png,
 * 1232x800, 240.1 KiB]" — used wherever pixels can't go (the transcript
 * log, adapters serializing for a model without image input). Caller
 * frees. */
char *item_image_placeholder(const struct item_image *img);

/* Aggregate limits on the images a conversation may hold, enforced at
 * ingestion (a `read` that would exceed either doesn't attach — see
 * image_budget_enforce) so history never accumulates past what the
 * strictest backend accepts and no request — including /compact — can
 * wedge on a permanently-rejected payload.
 *
 * Bytes: Anthropic rejects any request over 32 MB, and that ceiling covers
 * the whole JSON body — system prompt (including AGENTS.md), tool schemas,
 * and text history — not just the images. Budgeting images well under it
 * reserves a worst-case margin for that non-image content, so even a
 * /compact request (which resends the full history) stays admissible. 20 MB
 * of base64 is still several full-size images or the whole count cap of
 * small ones.
 *
 * Count: Anthropic allows at most 100 images per request AND drops the
 * per-image dimension limit from 8000px to 2000px once more than 20 are
 * present. Capping at 20 stays in that most-permissive tier — under the
 * 100 limit and preserving the full per-image dimension cap (see
 * READ_IMAGE_MAX_SIDE) — so no count-dependent dimension logic is needed. */
#define IMAGE_REQUEST_BUDGET_B64 ((size_t)20 * 1024 * 1024)
#define IMAGE_REQUEST_MAX_COUNT  20

/* Total base64 bytes / total image-part count across `items`. */
size_t images_total_b64(const struct item *items, size_t n);
size_t images_total_count(const struct item *items, size_t n);

struct tool_def {
    const char *name;
    const char *description;
    const char *parameters_schema_json; /* literal JSON Schema */
    const char *display_arg;            /* name of the JSON field shown after "[tool]" in the UI */
};

struct context {
    const char *system_prompt;
    const struct item *items;
    size_t n_items;
    const struct tool_def *tools;
    size_t n_tools;
    /* Optional. When non-NULL, providers pass it verbatim to whichever
     * field their API uses (Responses: reasoning.effort, Chat Completions:
     * reasoning_effort). NULL means "don't send" so the server picks its
     * own default. Values like "minimal"/"low"/"medium"/"high"/"xhigh" are
     * passed through unchanged — hax doesn't validate or clamp. */
    const char *effort;
    /* Does the target model accept image input? 1 yes, 0 no, -1 unknown.
     * Unknown is treated as yes throughout: a wrong yes surfaces as a
     * recoverable provider error, a wrong no silently drops content.
     * Filled by the agent (agent_image_input). Adapters serialize
     * tool-result image parts as native image blocks when nonzero and as
     * text placeholders when 0 — a resumed or model-switched conversation
     * must not send pixels to a text-only model. */
    int image_input;
};

/* Token accounting for one completed response. -1 means "not reported by
 * this provider/backend" — many OpenAI-compatible servers don't surface
 * cached_tokens, and some omit usage entirely. Callers must check for -1
 * before formatting. input_tokens is everything we just sent (system +
 * full history + the new user message); output_tokens is what was just generated.
 * "context used" for display = input + output, since both will be in the
 * next request's input. cached_tokens is a subset of input_tokens (prefix
 * cache hit) — informational, not additive. Note that cached_tokens means
 * cache *reads* only: dialects that bill cache writes separately
 * (Anthropic's cache_creation_input_tokens) fold the written tokens into
 * input_tokens, keeping that count volume-accurate, and report them again
 * in cache_write_tokens — a second, non-overlapping subset of
 * input_tokens — so cost estimation can price the write surcharge.
 * Dialects with no such billing notion leave it at -1.
 * cache_write_1h_tokens narrows further: the subset of cache_write_tokens
 * written with a 1-hour TTL, which Anthropic bills at 2x the input rate
 * (the catalog's cache_write rate covers only the default 5-minute
 * writes). -1 where the dialect doesn't break writes down by TTL.
 *
 * cost is the provider-reported charge for this response in USD (e.g.
 * OpenRouter's usage.cost when usage accounting is requested); negative
 * means not reported. Providers never compute cost from token prices
 * themselves — estimation from catalog rates is the agent's job. */
struct stream_usage {
    long input_tokens;
    long output_tokens;
    long cached_tokens;
    long cache_write_tokens;
    long cache_write_1h_tokens;
    double cost;
};

/* ITEM_TURN_USAGE payload: raw reported usage plus the costs resolved at
 * emission time (cost estimation is the agent layer's job — see
 * turn_usage_make — so display consumers stay pure formatting). */
struct turn_usage {
    struct stream_usage usage; /* -1 fields = not reported */
    long elapsed_ms;           /* stream wall time, retries included; -1 = unknown */
    /* USD. cost_total is the provider-reported charge when one was
     * reported (exact), else the catalog estimate, marked by
     * cost_estimated ("~$"); -1 = unknown. The per-category fields are
     * catalog estimates and stay -1 when the total is exact — a reported
     * charge arrives as one number and can't be decomposed. */
    double cost_in;          /* uncached input */
    double cost_cache_read;  /* prefix-cache reads */
    double cost_cache_write; /* cache writes, 1h surcharge included */
    double cost_out;
    double cost_total;
    int cost_estimated;
};

/* Events emitted by a provider's stream() into a stream_cb. */
enum stream_event_kind {
    EV_TEXT_DELTA,
    EV_TOOL_CALL_START, /* id + name known */
    EV_TOOL_CALL_DELTA, /* partial JSON args */
    EV_TOOL_CALL_END,   /* args finalized */
    EV_REASONING_ITEM,  /* opaque provider blob to round-trip on next turn */
    /* The model is currently producing reasoning/thinking tokens. Carries
     * the delta text (may be NULL/empty if the provider only signals the
     * state without exposing content, e.g. OpenAI o-series via Responses
     * API — such state-only deltas are ignored). Drives the "thinking..."
     * spinner, and the text is accumulated into an ITEM_REASONING
     * (reasoning_text). For opted-in openai-family providers that text is
     * replayed to the model as reasoning_content (see
     * openai_preset.roundtrip_reasoning_field); Codex instead round-trips
     * an opaque blob via EV_REASONING_ITEM and keeps the delta text only
     * for the transcript. */
    EV_REASONING_DELTA,
    /* A streaming HTTP attempt failed with a transient error and the
     * provider is about to retry. Pure UX signal — the agent uses it
     * to update the spinner label so the user sees that we're not
     * stuck on a dead connection. The eventual outcome (success or
     * EV_ERROR after exhausting retries) arrives later. */
    EV_RETRY,
    /* Prompt-processing (prefill) progress for this turn. Carries
     * llama.cpp-style counts: `processed`/`total` are tokens, `cache`
     * is the prefix-cache hit subset of `total` (so cache <=
     * processed <= total). The UX-meaningful fraction is
     * (processed - cache) / (total - cache), which starts at 0% each
     * turn regardless of cache reuse. Pure UX signal — never committed
     * to history. Only llama.cpp currently sources these; the openai
     * translation gates emission behind a per-preset flag (set
     * return_progress:true in the request body) so backends that don't
     * speak it never see synthesized events. */
    EV_PROGRESS,
    EV_DONE,
    EV_ERROR,
};

struct stream_event {
    enum stream_event_kind kind;
    union {
        struct {
            const char *text;
        } text_delta;
        struct {
            const char *id;
            const char *name;
        } tool_call_start;
        struct {
            const char *id;
            const char *args_delta;
        } tool_call_delta;
        struct {
            const char *id;
        } tool_call_end;
        struct {
            const char *json;
        } reasoning_item;
        struct {
            const char *text; /* NULL or "" allowed — signals reasoning
                                 activity even when no plaintext is exposed */
        } reasoning_delta;
        struct {
            int attempt;      /* 1-based attempt that just failed */
            int max_attempts; /* total attempts the provider will make */
            long delay_ms;    /* about to sleep this long before retrying */
            int http_status;  /* 0 = transport error, otherwise HTTP code */
        } retry;
        struct {
            long processed; /* tokens prefilled so far this turn */
            long total;     /* total prompt tokens to prefill */
            long cache;     /* subset of `total` served from prefix cache */
        } progress;
        struct {
            const char *stop_reason;
            struct stream_usage usage;
        } done;
        struct {
            const char *message;
            int http_status;
            /* Usage the provider reported before the failure, or NULL.
             * Truncated responses (max_tokens/length) are billed like
             * complete ones, so translators that captured usage attach
             * it here and the agent accounts it exactly as EV_DONE's. */
            const struct stream_usage *usage;
        } error;
    } u;
};

typedef int (*stream_cb)(const struct stream_event *ev, void *user);

struct provider {
    const char *name;
    /* Model id used when HAX_MODEL is unset. NULL = no safe default; the
     * agent will refuse to start and print an error. */
    const char *default_model;
    /* Optional presentation formatter for model ids. Returns a newly allocated
     * display label that the caller owns; the exact id is still used on the wire
     * and in persisted metadata. NULL means use the id unchanged. */
    char *(*model_label)(struct provider *p, const char *model);
    /* Reasoning effort used when HAX_EFFORT is unset. NULL means
     * omit the field and let the backend choose. */
    const char *default_effort;
    /* Sort list_models output alphabetically in the /model picker. Set by
     * providers whose catalog order carries no meaning; leave 0 where the
     * server's order is deliberate (curated, newest-first) or trivially
     * short. A default only — the global sort_models config key lets the
     * user force either order at the picker. */
    int sort_models;
    /* Identity in the model-metadata catalog (catalog.h): the models.dev
     * provider key this provider's model ids resolve under — "openai" for
     * both codex and openai, "anthropic" for anthropic. Drives catalog-based
     * cost estimation and the context/output-limit fallback. NULL opts out:
     * local backends whose models have no catalog presence, providers that
     * report exact cost and probe their own limits (openrouter), and
     * config-defined providers that opted out (their default is their own
     * name — see config_provider.c). Borrowed static/config string that
     * outlives the provider. */
    const char *catalog_id;
    /* Stream a model response. The provider drives the HTTP round-trip
     * and translates SSE events into stream_event callbacks (`cb`). The
     * `tick` slot is the agent's side-channel hook into the wait loop —
     * called periodically (~1Hz) and on each received chunk, with the
     * agent's user pointer. Returning non-zero aborts the transfer.
     * Providers thread it straight through to http_sse_post; the mock
     * provider polls it from its sleep loop so scripted pauses exercise
     * the same path. NULL tick disables the hook. */
    int (*stream)(struct provider *p, const struct context *ctx, const char *model, stream_cb cb,
                  void *user, http_tick_cb tick, void *tick_user);
    /* Optional. Print a provider-specific subscription/usage report to
     * stdout (rate-limit windows for a Codex-style plan, paid-API spend
     * totals, etc.). NULL means "/usage is not supported on this
     * provider". Returns 0 on success, -1 on fetch/parse failure (the
     * implementation is expected to have already printed a diagnostic
     * to stderr in that case). Implementations run their blocking
     * fetches under a busy window (busy.h) so a spinner shows and Esc
     * cancels; a cancelled fetch returns -1 with no diagnostic. */
    int (*query_usage)(struct provider *p);
    /* Optional. Discover the model ids this provider can serve, for the
     * runtime model picker (/model). Returns 0 with *ids a freshly-allocated
     * array of *n heap-owned strings (caller frees; *n may be 0), or -1 on
     * any failure with *ids=NULL and *err set to a malloc'd user-actionable
     * diagnostic ("codex token expired — …", "could not reach <name> at
     * <url>") that the caller prints and frees. Set *err on every failure
     * path: only the adapter can name the endpoint and remedy; the caller's
     * fallback for an unset *err is a bare generic line. When no menu can
     * be built the selector prints a note and skips the model step; a
     * failure (-1) or an empty catalog (*n==0) also rolls a /provider
     * switch back entirely, while a NULL hook — the provider can't
     * enumerate models — lets the switch proceed on the new provider's
     * default_model. The picker calls this synchronously on the foreground
     * path, so implementations must bound the network round-trip with a
     * short timeout AND thread `tick` (same shape as stream()'s, may be
     * NULL) into it — the picker cancels the fetch through it when the
     * user presses Esc. */
    int (*list_models)(struct provider *p, char ***ids, size_t *n, char **err, http_tick_cb tick,
                       void *tick_user);
    /* Optional. Reasoning-effort wire values this provider accepts (e.g.
     * "low", "high"), for the runtime effort picker (/effort). Points *out
     * at a borrowed array (typically static, owned by the provider; valid
     * for the provider's lifetime) and returns its length. Returns 0 (or a
     * NULL hook) when the provider has no categorical reasoning effort — its
     * thinking is a token budget/toggle, or it has none — in which case the
     * picker reports "not supported" and the chained flow skips the step.
     * The selector prepends its own "default (omit)" choice, so providers
     * list only real effort values here. */
    size_t (*list_efforts)(struct provider *p, const char *const **out);
    /* Optional. Re-run the model-specific context-window probe after a
     * runtime /model switch, so context_limit tracks the newly selected
     * model rather than the one resolved at construction. Implementations
     * cancel/join any in-flight probe, reset context_limit to 0 (unknown),
     * and spawn a fresh probe for `model`. NULL when the limit isn't
     * model-specific — fixed, absent, or sourced only from
     * HAX_CONTEXT_LIMIT (which the agent honors ahead of this slot anyway).
     * codex and openrouter key the window by model via their catalogs;
     * llama.cpp re-probes /props for the selected model (router mode serves
     * several, differing in window and vision capability). Providers whose
     * window is fixed or absent leave it NULL. */
    void (*refresh_context)(struct provider *p, const char *model);
    void (*destroy)(struct provider *p);
    /* Auto-discovered model context window in tokens. 0 = unknown (no
     * probe ran, hasn't completed yet, or failed); positive values are
     * what the agent's status line uses to render the "%" of context
     * used. Updated atomically — provider construction may set it
     * synchronously from HAX_CONTEXT_LIMIT, or a background probe owned
     * by the implementation may write it once the catalog response
     * arrives. The agent reads it on every usage update, so a
     * late-landing probe fills in the percentage starting with
     * whichever turn it completes during. Implementations own the
     * worker that writes here and are responsible for joining it in
     * their destroy() before any teardown that could free this slot. */
    _Atomic long context_limit;
    /* Auto-discovered image-input capability of the active model, when
     * the backend itself can answer (llama-server /props, OpenRouter
     * /endpoints). enum provider_image_input values; UNKNOWN (0, the
     * calloc default) falls back to the catalog. A live answer beats the
     * catalog — llama.cpp vision depends on the mmproj loaded on this
     * server instance, which no catalog can know. Same atomicity and
     * ownership rules as context_limit. */
    _Atomic long image_input;
};

enum provider_image_input {
    PROVIDER_IMG_UNKNOWN = 0,
    PROVIDER_IMG_YES = 1,
    PROVIDER_IMG_NO = 2,
};

/* Static provider descriptor. Each provider .c file defines exactly one
 * `const struct provider_factory PROVIDER_<NAME>` symbol; main.c collects
 * them into a registry and matches HAX_PROVIDER against `name`. The
 * default when HAX_PROVIDER is unset lives in main.c's DEFAULT_PROVIDER
 * constant, decoupled from registry order. */
struct provider_factory {
    const char *name; /* HAX_PROVIDER value, e.g. "codex", "llama.cpp" */
    /* Construct the provider. `name` is this factory's own `name` field,
     * threaded in so a single generic constructor can serve many config-
     * defined providers (each a distinct name ⇒ a distinct providers.<name>.*
     * config subtree). The compiled-in factories each serve one fixed
     * provider and ignore the argument. */
    struct provider *(*new)(const char *name);
    /* Optional availability check for the runtime provider picker
     * (/provider): is this provider expected to work right now? Advisory,
     * not gating — the picker shows a failing provider dim with *reason
     * ("OPENAI_API_KEY not set", "server not reachable") but still lets it
     * be picked, re-running this check at commit time to report the fresh
     * verdict. Returns 1 when usable, 0 when not (then may set *reason to
     * a short static string). May perform a bounded network probe (a local
     * server's reachability GET); the picker runs these off the foreground
     * path and in parallel so opening the list stays fast even when a host
     * hangs. The reason string, if set, must outlive the call (use static
     * literals — these run on worker threads). `name` is the factory's own
     * name (see `new`). NULL hook ⇒ always available. */
    int (*available)(const char *name, const char **reason);
    /* Dev-only backend, hidden from the enumerated provider set: excluded
     * from the /provider picker, cold-start auto-selection, and the
     * "supported" list in error messages. Still resolvable by name, so it
     * stays reachable via an explicit HAX_PROVIDER=<name> (e.g. the mock
     * provider for manual pipeline testing). */
    int internal;
};

#endif /* HAX_PROVIDER_H */
