/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDER_H
#define HAX_PROVIDER_H

#include <stddef.h>

#include "catalog.h"
#include "effort.h"
#include "transport/http.h"

/* Flat, provider-independent conversation records. Adapters may group adjacent records when their
 * wire format requires compound assistant messages or tool-result blocks. */
enum item_kind {
    ITEM_USER_MESSAGE,
    ITEM_ASSISTANT_MESSAGE,
    ITEM_TOOL_CALL,
    ITEM_TOOL_RESULT,
    /* May carry opaque provider state, displayable text, or both. */
    ITEM_REASONING,
    /* Fieldless marker before each model request; adapters do not serialize it. */
    ITEM_TURN_BOUNDARY,
    /* Usage for the preceding model request; adapters do not serialize it. */
    ITEM_TURN_USAGE,
};

/* Agent-authored provenance cannot be inferred from content because users, models, and tools may
 * reproduce the same marker text. Zero denotes an ordinary, externally authored item. */
enum item_origin {
    ITEM_ORIGIN_NONE = 0,
    ITEM_ORIGIN_COMPACT_SEED, /* synthetic USER_MESSAGE summarizing the history before it */
    ITEM_ORIGIN_CONTINUATION, /* synthetic USER_MESSAGE after an interrupted turn */
    ITEM_ORIGIN_INTERRUPTED,  /* ASSISTANT_MESSAGE marked as interrupted */
    ITEM_ORIGIN_SKIPPED,      /* TOOL_RESULT for a call that did not run after an abort */
    ITEM_ORIGIN_REFUSED,      /* TOOL_RESULT for a call disabled by the frontend */
    ITEM_ORIGIN_SUMMARIZED,   /* TOOL_RESULT standing in for separately displayed output */
    ITEM_ORIGIN_TASK_NOTE,    /* synthetic USER_MESSAGE reporting finished background tasks */
};

/* One inline image carried by an item: base64 payload plus the metadata
 * adapters, display, and persistence need. Dimensions are parsed from the
 * image header at ingestion (0 = unknown). */
struct item_image {
    char *mime;     /* owned MIME type */
    char *data_b64; /* owned base64 payload */
    long width;     /* pixels; 0 = unknown */
    long height;    /* pixels; 0 = unknown */
};

/* Pointer fields are owned by the item and released by item_free. */
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
    /* TOOL_RESULT: length of a trailing model-only annotation in `output`; user-facing
     * rendering omits it. */
    size_t output_hidden_tail;
    /* TOOL_RESULT: owned image parts attached alongside `output`. */
    struct item_image *images;
    size_t n_images;
    /* REASONING: opaque provider state ready to send back unchanged. */
    char *reasoning_json;
    /* REASONING: human-readable reasoning; an item may carry either form or both. */
    char *reasoning_text;
    /* REASONING / TURN_USAGE: source identity. Opaque reasoning may be bound to this exact pair. */
    char *provider;
    char *model;
    enum item_origin origin;
    /* TURN_USAGE: owned accounting payload; NULL for other kinds. */
    struct turn_usage *usage;
};

/* Release all fields owned by `item`. The item must not be used afterward. NULL-safe. */
void item_free(struct item *item);

/* Return an allocated one-line text replacement for an image part. */
char *item_image_placeholder(const struct item_image *image);

/* Keeping at most 20 images preserves Anthropic's larger per-image dimension limit. Reserving only
 * 20 MiB of base64 under its 32 MB body limit leaves room for prompts, tools, and text history. */
#define IMAGE_REQUEST_BASE64_BUDGET_BYTES ((size_t)20 * 1024 * 1024)
#define IMAGE_REQUEST_MAX_COUNT           20

size_t items_image_base64_bytes(const struct item *items, size_t n_items);
size_t items_image_count(const struct item *items, size_t n_items);

/* Index of the newest compaction seed, or 0 when there is none. Compaction summarizes a prefix
 * instead of discarding it, so a request built from `items` starts here, not at 0. */
size_t items_context_floor(const struct item *items, size_t n_items);

/* One JSON Schema property advertised for a tool. Zeroed optional fields are omitted from the
 * generated schema, so `minimum` cannot express a bound of 0. */
struct tool_param {
    const char *name;
    const char *type;      /* JSON Schema primitive: "string", "integer", "boolean", or "array" */
    const char *item_type; /* element type when `type` is "array" */
    const char *description;
    int required;
    long minimum;
};

struct tool_def {
    const char *name;
    const char *description;
    const struct tool_param *params;
    size_t n_params;
};

struct context {
    const char *system_prompt;
    const struct item *items;
    size_t n_items;
    const struct tool_def *tools;
    size_t n_tools;
    /* Wire reasoning-effort value, or NULL to let the provider choose. */
    const char *effort;
    /* Image-input capability: 1 yes, 0 no, -1 unknown. Unknown is treated as yes because rejecting
     * an image is recoverable, while suppressing one silently loses content. */
    int image_input;
};

/* Usage reported for one response. Negative values mean unreported. `cached_tokens` and
 * `cache_write_tokens` are normally subsets of `input_tokens`, but providers may report overlapping
 * cache reads and writes; consumers must not assume those categories subtract cleanly. The 1-hour
 * count is a subset of cache writes. `cost` is a provider-reported USD charge, never an
 * estimate. */
struct stream_usage {
    long input_tokens;
    long output_tokens;
    long cached_tokens;
    long cache_write_tokens;
    long cache_write_1h_tokens;
    double cost;
};

/* Identity of the response behind one turn, as the provider reported it. Borrowed for the
 * callback; NULL means unreported. */
struct stream_response {
    const char *id;
    const char *model; /* may resolve an alias or a router's choice */
    const char *route; /* upstream endpoint a gateway selected */
};

/* Display identity for one ITEM_TURN_USAGE record. Labels are stored rather than resolved when
 * rendering because a model label is a provider-instance convention. */
struct turn_provenance {
    /* NULL when the item's wire provider/model already reads the same. */
    char *provider_label;
    char *model_label;
    /* Wire effort sent with the request; kept per turn because it changes how the model answers
     * and can invalidate the prompt cache. */
    char *effort;
    char *served_model; /* NULL unless the response named a model other than the request's */
    char *route;
    char *response_id; /* recorded for correlation; not rendered */
};

/* Accounting payload stored in an ITEM_TURN_USAGE record. Costs are USD; negative values are
 * unknown. Category costs are always estimates. The total prefers an exact provider-reported cost,
 * with `cost_estimated` indicating when it is an estimate instead. */
struct turn_usage {
    struct stream_usage usage;
    long elapsed_ms;            /* stream wall time, including retries; -1 = unknown */
    long uncached_input_tokens; /* count priced by cost_input; -1 = unknown */
    double cost_input;
    double cost_cache_read;
    double cost_cache_write; /* includes the 1-hour surcharge */
    double cost_output;
    double cost_total;
    int cost_estimated;
    struct turn_provenance provenance;
};

/* Release `usage` and every field it owns. NULL-safe. */
void turn_usage_free(struct turn_usage *usage);

/* Events emitted by a provider's stream(). Pointer payloads are borrowed and remain valid only for
 * the callback invocation. */
enum stream_event_kind {
    EV_TEXT_DELTA,
    EV_TOOL_CALL_START, /* id + name known */
    EV_TOOL_CALL_DELTA, /* partial JSON args */
    EV_TOOL_CALL_END,   /* args finalized */
    EV_REASONING_ITEM,  /* opaque provider state to preserve for the next request */
    EV_REASONING_DELTA, /* text may be NULL or empty to signal activity without exposing content */
    EV_RETRY,           /* transient attempt failed; another will follow */
    EV_PROGRESS,        /* prompt-prefill progress; not committed to history */
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
            /* Usage the failed attempt reported before dying, or NULL. A retried
             * attempt is billed like a completed one, so consumers add it to the
             * turn's accounted total. */
            const struct stream_usage *usage;
        } retry;
        struct {
            long processed; /* tokens prefilled so far this turn */
            long total;     /* total prompt tokens to prefill */
            long cache;     /* subset of `total` served from prefix cache */
        } progress;
        struct {
            const char *stop_reason;
            struct stream_usage usage;
            struct stream_response response; /* zeroed when nothing was identified */
        } done;
        struct {
            const char *message;
            int http_status;
            /* Usage the provider reported before the failure, or NULL.
             * Truncated responses (max_tokens/length) are billed like
             * complete ones, so translators that captured usage attach
             * it here and the agent accounts it exactly as EV_DONE's. */
            const struct stream_usage *usage;
            /* Identity captured before the failure, or NULL; a retained
             * footer owes the same attribution as EV_DONE's. */
            const struct stream_response *response;
        } error;
    } u;
};

typedef int (*stream_cb)(const struct stream_event *ev, void *user);

/* Tri-state capability answer. Zero is unknown so zero-initialized metadata is valid. */
enum provider_cap {
    PROVIDER_CAP_UNKNOWN = 0,
    PROVIDER_CAP_YES = 1,
    PROVIDER_CAP_NO = 2,
};

/* Raw metadata reported by a provider for one model. Every field after `id` is optional;
 * model_info_init establishes the unknown sentinels. Resolved metadata belongs in model_meta.h. */
struct model_info {
    char *id;          /* owned exact wire id; NULL in metadata-only merged views */
    char *description; /* owned one-line description; NULL when absent */
    long context;      /* served context window in tokens; 0 = unknown */
    long max_context;  /* provider-declared ceiling for context overrides; 0 = unknown */
    long max_output;   /* maximum output tokens per response; 0 = unknown */
    enum provider_cap image_input;
    enum provider_cap tools;
    /* Provider-reported USD rates per million tokens; negative = unknown. */
    double cost_input;
    double cost_cache_read;
    double cost_output;
    double cost_cache_write;
    double cost_cache_write_1h;
    /* Long-context pricing tiers in provider order. */
    struct catalog_tier tiers[CATALOG_TIERS_MAX];
    int n_tiers;
    /* Accepted wire effort values, not provider-specific display labels. */
    struct effort_set efforts;
};

/* Initialize `info` with all fields unknown. */
void model_info_init(struct model_info *info);

/* Deep-copy `src` into `dst` (which must not already own anything). */
void model_info_copy(struct model_info *dst, const struct model_info *src);

/* Release fields owned by `info` and leave it zeroed. */
void model_info_clear(struct model_info *info);

/* Clear `n_models` entries and free the array. NULL-safe. */
void model_info_free(struct model_info *models, size_t n_models);

/* Prepared single-model metadata request. Request construction stays on the foreground thread;
 * only these owned fields and the pure parser cross into the background worker. */
struct model_probe {
    char *url;      /* owned */
    char **headers; /* owned, NULL-terminated; NULL = none */
    int timeout_s;
    /* Locate `model` in the response and fill initialized `out`. Runs off-thread without access to
     * the provider, so implementations must be pure and thread-safe. */
    void (*parse)(const char *body, const char *model, struct model_info *out);
};

/* Release all request fields owned by `probe` and leave it zeroed. NULL-safe. */
void model_probe_clear(struct model_probe *probe);

struct model_meta; /* model_meta.h; opaque provider-owned storage */

struct provider {
    const char *name;
    /* Stable selectable id (the factory name). Reasoning provenance and replay matching must use
     * this, never `name`: display labels can change or collide across providers. */
    const char *id;
    /* NULL when no safe default exists. Only values derived from live state (server discovery, a
     * companion tool's config) qualify; compiled-in model names go stale in shipped binaries. */
    const char *default_model;
    /* Return an allocated display label, or NULL to use the wire id unchanged. */
    char *(*model_label)(struct provider *provider, const char *model);
    const char *default_effort; /* NULL omits reasoning effort */
    /* The active model was detected from transient server state and must not be persisted. */
    int model_discovered;
    /* Keep the picker in server order instead of the default version-aware sort; user
     * configuration may override either way. */
    int keep_model_order;
    /* models.dev provider key used for metadata fallback. NULL opts out. A dynamically resolved key
     * must be owned by the provider because configuration storage may be replaced at runtime. */
    const char *catalog_id;
    /* Emit provider-independent events for one response. `tick` is called periodically and on
     * received data; a nonzero return aborts the transfer. NULL disables ticking. */
    int (*stream)(struct provider *provider, const struct context *context, const char *model,
                  stream_cb callback, void *user, http_tick_cb tick, void *tick_user);
    /* Print a provider-specific usage report. Return 0 on success or -1 after reporting an error;
     * cancellation also returns -1 but prints nothing. NULL means unsupported. */
    int (*query_usage)(struct provider *provider);
    /* Return a newly allocated model array, or -1 with `*models == NULL` and an allocated,
     * user-actionable `*error`. The caller clears successful arrays with model_info_free and frees
     * errors. Network implementations must use a short timeout and honor `tick`. NULL means the
     * provider cannot enumerate models. */
    int (*list_models)(struct provider *provider, struct model_info **models, size_t *n_models,
                       char **error, http_tick_cb tick, void *tick_user);
    /* Return a borrowed array of accepted wire effort values. Zero or NULL means unsupported. */
    size_t (*list_efforts)(struct provider *provider, const char *const **efforts);
    /* Prepare a small metadata request for one model. Return 0 with owned request fields, or -1
     * when nothing can be fetched. NULL leaves metadata resolution to the catalog. */
    int (*probe_model)(struct provider *provider, const char *model, struct model_probe *probe);
    void (*destroy)(struct provider *provider);
    /* Every destroy callback must call model_meta_release before freeing the provider. */
    struct model_meta *meta;
};

/* Prepared provider availability check. A NULL `url` makes `available` the immediate verdict;
 * otherwise the owned GET request may be passed safely to a worker. */
struct provider_availability {
    int available;
    char *reason; /* owned unavailable reason; NULL when available */
    char *url;
    char **headers; /* owned, NULL-terminated; NULL = none */
    long timeout_s;
};

/* Release owned request fields and leave `availability` zeroed. NULL-safe. */
void provider_availability_clear(struct provider_availability *availability);

/* Map a former provider id to its current one so saved sessions and scripts keep resolving;
 * unrecognized names (including NULL) pass through unchanged. */
const char *provider_canonical_id(const char *name);

/* Provenance identity: the stable id, or the display name for bare providers without one. */
const char *provider_stable_id(const struct provider *provider);

/* Whether an item's recorded provenance names this provider/model pair. Sessions written by
 * earlier versions may carry a former provider id, so both sides are canonicalized. */
int provider_provenance_matches(const struct item *item, const char *provider, const char *model);

#endif /* HAX_PROVIDER_H */
