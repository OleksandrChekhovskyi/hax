/* SPDX-License-Identifier: MIT */
#include "openai.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "openai_events.h"
#include "util.h"
#include "system/bg_job.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

/* Short timeouts for the runtime pickers: the availability probe must keep
 * /provider fast even when a server hangs (the 2s connect timeout in
 * http_get bounds the unreachable case); the model-list fetch tolerates a
 * larger catalog (OpenRouter's is hundreds of entries). */
#define AVAIL_PROBE_TIMEOUT_S 2
#define MODEL_LIST_TIMEOUT_S  10

const char *const OPENAI_EFFORT_LADDER[] = {"none", "minimal", "low", "medium", "high", "xhigh"};
const size_t OPENAI_EFFORT_LADDER_N =
    sizeof(OPENAI_EFFORT_LADDER) / sizeof(OPENAI_EFFORT_LADDER[0]);

struct openai {
    struct provider base;
    char *base_url;   /* e.g. "http://127.0.0.1:8000/v1" (no trailing slash) */
    char *api_key;    /* may be NULL for unauthenticated local servers */
    char *name_buf;   /* backing storage for base.name (heap-owned) */
    char *endpoint;   /* base_url + "/chat/completions" */
    char *session_id; /* sent as prompt_cache_key when send_cache_key is set */
    int send_cache_key;
    int emit_progress;
    int request_cost;
    const char *length_hint;         /* borrowed; appended to "length" truncation errors */
    char *roundtrip_reasoning_field; /* NULL = don't round-trip reasoning */
    enum reasoning_format reasoning_format;
    const char *const *efforts; /* borrowed effort ladder for /effort; NULL = none */
    size_t n_efforts;
    char **extra_headers; /* NULL-terminated, each element heap-owned; NULL = none */
    /* Optional background probe — currently the context-window
     * discovery spawned by preset shims (openrouter, llama.cpp).
     * Joined by openai_destroy below. */
    struct bg_job *probe;
};

/* ---------- request body construction ---------- */

static json_t *build_tool_call(const struct item *it)
{
    return json_pack("{s:s, s:s, s:{s:s, s:s}}", "id", it->call_id ? it->call_id : "", "type",
                     "function", "function", "name", it->tool_name ? it->tool_name : "",
                     "arguments", it->tool_arguments_json ? it->tool_arguments_json : "{}");
}

/* Emit one consolidated assistant message for a run of consecutive
 * ITEM_REASONING + ITEM_ASSISTANT_MESSAGE + ITEM_TOOL_CALL items. OpenAI
 * accepts a single assistant message with both `content` and `tool_calls`;
 * some compat servers reject bare tool_call-only messages without that
 * wrapper.
 *
 * When `reasoning_field` is set, any captured reasoning text in the run is
 * attached under that key (e.g. "reasoning_content" for llama.cpp) so
 * interleaved-thinking models see their own prior CoT — see openai.h. Only
 * ITEM_REASONING.reasoning_text is round-tripped here; Codex's structured
 * reasoning_json travels its own provider and never reaches this path.
 *
 * A reasoning item is replayed only when its provenance stamp matches the
 * live `cur_provider`/`cur_model` — otherwise CoT produced by Codex, or by a
 * different llama.cpp model earlier in the same conversation, would be fed to
 * a backend/model that never wrote it. Unstamped items (NULL provider/model)
 * are conservatively skipped; for resumed pre-stamp files session_load has
 * already backfilled the stamp from the header.
 *
 * Lossy corner: if a turn produced text → tool_call → text, both text
 * fragments are concatenated into one `content` — Chat Completions has
 * no way to interleave text with tool_calls within a message. Splitting
 * the trailing text into a post-tool-result message would imply it was
 * said after observing the result, which is more misleading. */
static size_t emit_assistant_group(json_t *arr, const struct item *items, size_t i, size_t n,
                                   const char *reasoning_field, const char *cur_provider,
                                   const char *cur_model)
{
    struct buf text;
    struct buf reasoning;
    buf_init(&text);
    buf_init(&reasoning);
    json_t *tool_calls = NULL;

    while (i < n && (items[i].kind == ITEM_ASSISTANT_MESSAGE || items[i].kind == ITEM_TOOL_CALL ||
                     items[i].kind == ITEM_REASONING)) {
        if (items[i].kind == ITEM_ASSISTANT_MESSAGE) {
            if (items[i].text)
                buf_append_str(&text, items[i].text);
        } else if (items[i].kind == ITEM_REASONING) {
            int stamp_ok = items[i].provider && items[i].model && cur_provider && cur_model &&
                           strcmp(items[i].provider, cur_provider) == 0 &&
                           strcmp(items[i].model, cur_model) == 0;
            if (stamp_ok && items[i].reasoning_text && *items[i].reasoning_text) {
                if (reasoning.len > 0)
                    buf_append_str(&reasoning, "\n");
                buf_append_str(&reasoning, items[i].reasoning_text);
            }
        } else {
            if (!tool_calls)
                tool_calls = json_array();
            json_array_append_new(tool_calls, build_tool_call(&items[i]));
        }
        i++;
    }

    int emit_reasoning = reasoning_field && reasoning.len > 0;

    /* Skip an assistant message with nothing to say — no text, no tool
     * calls, and no reasoning being replayed. Happens for a reasoning-only
     * turn when replay is disabled (reasoning_field NULL: HAX_REASONING_
     * ROUNDTRIP=off, or a compat backend that streams CoT but doesn't opt
     * into replay). Emitting {"role":"assistant","content":null} there
     * poisons the next request and some backends reject it. */
    if (text.len == 0 && !tool_calls && !emit_reasoning) {
        buf_free(&text);
        buf_free(&reasoning);
        return i;
    }

    json_t *msg = json_object();
    json_object_set_new(msg, "role", json_string("assistant"));
    if (text.len > 0)
        json_object_set_new(msg, "content", json_string(text.data));
    else
        json_object_set_new(msg, "content", json_null());
    if (tool_calls)
        json_object_set_new(msg, "tool_calls", tool_calls);
    if (emit_reasoning)
        json_object_set_new(msg, reasoning_field, json_string(reasoning.data));
    json_array_append_new(arr, msg);

    buf_free(&text);
    buf_free(&reasoning);
    return i;
}

json_t *openai_build_messages(const char *system_prompt, const struct item *items, size_t n,
                              const char *reasoning_field, const char *cur_provider,
                              const char *cur_model)
{
    json_t *arr = json_array();

    if (system_prompt && *system_prompt) {
        json_array_append_new(arr,
                              json_pack("{s:s, s:s}", "role", "system", "content", system_prompt));
    }

    size_t i = 0;
    while (i < n) {
        switch (items[i].kind) {
        case ITEM_USER_MESSAGE:
            json_array_append_new(arr, json_pack("{s:s, s:s}", "role", "user", "content",
                                                 items[i].text ? items[i].text : ""));
            i++;
            break;
        case ITEM_ASSISTANT_MESSAGE:
        case ITEM_TOOL_CALL:
            i = emit_assistant_group(arr, items, i, n, reasoning_field, cur_provider, cur_model);
            break;
        case ITEM_TOOL_RESULT:
            json_array_append_new(arr,
                                  json_pack("{s:s, s:s, s:s}", "role", "tool", "tool_call_id",
                                            items[i].call_id ? items[i].call_id : "", "content",
                                            items[i].output ? items[i].output : ""));
            i++;
            break;
        case ITEM_REASONING:
            /* Text reasoning begins an assistant group so it can ride along
             * as reasoning_content (gated by reasoning_field). Codex's
             * structured blob has no Chat Completions representation — skip. */
            if (items[i].reasoning_text)
                i = emit_assistant_group(arr, items, i, n, reasoning_field, cur_provider,
                                         cur_model);
            else
                i++;
            break;
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            /* Agent-side markers for the transcript renderer; nothing to
             * translate. emit_assistant_group's while clause already
             * stops at these kinds, so they cleanly end an assistant
             * group too. */
            i++;
            break;
        }
    }

    return arr;
}

static json_t *build_tools(const struct tool_def *tools, size_t n)
{
    json_t *arr = json_array();
    for (size_t i = 0; i < n; i++) {
        json_error_t err;
        json_t *params = json_loads(tools[i].parameters_schema_json, 0, &err);
        if (!params) {
            hax_warn("bad tool schema for %s: %s", tools[i].name, err.text);
            params = json_object();
        }
        json_array_append_new(arr, json_pack("{s:s, s:{s:s, s:s, s:o}}", "type", "function",
                                             "function", "name", tools[i].name, "description",
                                             tools[i].description, "parameters", params));
    }
    return arr;
}

enum reasoning_format reasoning_format_parse(const char *s, enum reasoning_format fallback)
{
    if (!s || !*s)
        return fallback;
    if (strcasecmp(s, "flat") == 0)
        return REASONING_FLAT;
    if (strcasecmp(s, "nested") == 0)
        return REASONING_NESTED;
    hax_warn("unknown reasoning format %s (expected 'flat' or 'nested') — using default", s);
    return fallback;
}

void openai_apply_reasoning(json_t *body, enum reasoning_format fmt, const char *effort)
{
    /* Empty counts as unset (the "empty omits effort" contract): both dialects
     * then omit the field, leaving the provider's own default. */
    if (!effort || !*effort)
        return;
    switch (fmt) {
    case REASONING_FLAT:
        json_object_set_new(body, "reasoning_effort", json_string(effort));
        break;
    case REASONING_NESTED: {
        /* Nested `reasoning` object. `enabled` is the explicit on/off gate some
         * routers need to (de)activate reasoning: "none" sends enabled:false to
         * disable, any real effort sends enabled:true and passes the level
         * through. */
        int on = strcmp(effort, "none") != 0;
        json_t *r = json_pack("{s:b}", "enabled", on);
        if (on)
            json_object_set_new(r, "effort", json_string(effort));
        json_object_set_new(body, "reasoning", r);
        break;
    }
    }
}

static char *build_body(const struct context *ctx, const char *provider, const char *model,
                        const char *cache_key, enum reasoning_format reasoning, int return_progress,
                        int request_cost, const char *reasoning_field)
{
    /* Omit `tool_choice` and `parallel_tool_calls`: their defaults ("auto"
     * and true respectively) are exactly what we want, so explicitly setting
     * them would be redundant. The agent loop already handles multiple
     * tool_calls per turn when the model emits them.
     *
     * `stream_options.include_usage` is sent unconditionally — it asks the
     * server to emit a trailing usage chunk so we can show per-turn token
     * counts. Reference clients send it without per-call gating: opencode
     * always-on for streaming, pi-mono default-true via per-model compat
     * flag. Modern OpenAI-compatible backends (vLLM, llama.cpp server,
     * Ollama, oMLX, hosted providers) all accept it. If we ever hit a
     * backend that 400s on the unknown field, gating goes here. */
    json_t *body = json_pack("{s:s, s:b, s:o, s:{s:b}}", "model", model, "stream", 1, "messages",
                             openai_build_messages(ctx->system_prompt, ctx->items, ctx->n_items,
                                                   reasoning_field, provider, model),
                             "stream_options", "include_usage", 1);

    if (ctx->n_tools > 0)
        json_object_set_new(body, "tools", build_tools(ctx->tools, ctx->n_tools));

    if (cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(cache_key));

    /* llama.cpp extension: asks the server to inject prompt_progress
     * objects into the stream during prefill. Other OpenAI-compatible
     * backends ignore the unknown field, so we can send it whenever the
     * preset opts in without per-backend gating beyond that. */
    if (return_progress)
        json_object_set_new(body, "return_progress", json_true());

    /* OpenRouter extension: opt into usage accounting so the trailing
     * usage chunk includes this response's `cost` (USD). */
    if (request_cost)
        json_object_set_new(body, "usage", json_pack("{s:b}", "include", 1));

    openai_apply_reasoning(body, reasoning, ctx->effort);

    char *s = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return s;
}

/* ---------- SSE glue ---------- */

static int on_sse(const char *event_name, const char *data, void *user)
{
    (void)event_name; /* Chat Completions streams are unnamed `data:` events */
    openai_events_feed(user, data);
    return 0;
}

/* ---------- provider interface ---------- */

static int openai_stream(struct provider *p, const struct context *ctx, const char *model,
                         stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    struct openai *o = (struct openai *)p;

    char *body = build_body(ctx, p->name, model, o->send_cache_key ? o->session_id : NULL,
                            o->reasoning_format, o->emit_progress, o->request_cost,
                            o->roundtrip_reasoning_field);
    if (!body)
        return -1;
    size_t body_len = strlen(body);

    char *auth_hdr = o->api_key ? xasprintf("Authorization: Bearer %s", o->api_key) : NULL;

    /* Assemble headers: optional Authorization, the two fixed Accept/
     * Content-Type lines, and any preset-supplied extras (e.g. OpenRouter's
     * X-Title). Built dynamically because the extras count is variable. */
    size_t n_extra = 0;
    for (char **h = o->extra_headers; h && *h; h++)
        n_extra++;
    const char **headers = xmalloc(sizeof(*headers) * (n_extra + 4));
    size_t hi = 0;
    if (auth_hdr)
        headers[hi++] = auth_hdr;
    headers[hi++] = "Accept: text/event-stream";
    headers[hi++] = "Content-Type: application/json";
    for (char **h = o->extra_headers; h && *h; h++)
        headers[hi++] = *h;
    headers[hi] = NULL;

    struct retry_policy pol = retry_policy_default();
    struct http_response resp;
    struct openai_events ev;
    int rc = -1;

    /* Auto-retry on transient transport / 5xx / 429 errors. We re-init the
     * events parser each attempt so a partial parser state from a 5xx body
     * (which isn't SSE-shaped anyway) doesn't leak into the next try. The
     * request body and headers are immutable — same call_id, same prompt
     * cache key — so resending them is safe. */
    int attempt;
    for (attempt = 0; attempt < pol.max_attempts; attempt++) {
        memset(&resp, 0, sizeof(resp));
        openai_events_init(&ev, cb, user);
        ev.emit_progress = o->emit_progress;
        ev.length_hint = o->length_hint;
        rc = http_sse_post(o->endpoint, headers, body, body_len, on_sse, &ev, tick, tick_user,
                           &resp);

        if (resp.cancelled)
            break;
        if (!retry_should_attempt(rc, resp.status, resp.error_body))
            break;
        if (attempt + 1 >= pol.max_attempts)
            break;

        long delay = resp.retry_after_ms > 0 ? resp.retry_after_ms : retry_delay_ms(&pol, attempt);
        struct stream_event re = {
            .kind = EV_RETRY,
            .u.retry = {.attempt = attempt + 1,
                        .max_attempts = pol.max_attempts,
                        .delay_ms = delay,
                        .http_status = (int)resp.status},
        };
        cb(&re, user);

        free(resp.error_body);
        resp.error_body = NULL;
        openai_events_free(&ev);

        if (retry_sleep_with_tick(delay, tick, tick_user)) {
            /* Cancelled mid-backoff — synthesize the cancelled flag so
             * the post-loop branch below treats this like a user abort. */
            resp.cancelled = 1;
            memset(&ev, 0, sizeof(ev));
            break;
        }
    }

    free(headers);

    if (resp.cancelled) {
        /* User-initiated abort — agent layer handles the partial state
         * and the "[interrupted]" notice. Don't surface as EV_ERROR. */
    } else if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        char *msg = format_api_error(resp.status, resp.error_body);
        struct stream_event e = {
            .kind = EV_ERROR,
            .u.error = {.message = msg, .http_status = (int)resp.status},
        };
        cb(&e, user);
        free(msg);
    } else {
        openai_events_finalize(&ev);
    }

    free(resp.error_body);
    free(auth_hdr);
    free(body);
    openai_events_free(&ev);
    return rc;
}

static void openai_destroy(struct provider *p)
{
    struct openai *o = (struct openai *)p;
    /* Settle any background probe before freeing its target. Cancel
     * first so the worker exits its http_get promptly via the bg
     * cancel thunk wired through the progress callback. */
    if (o->probe) {
        bg_job_cancel(o->probe);
        bg_job_join(o->probe);
    }
    free(o->base_url);
    free(o->api_key);
    free(o->name_buf);
    free(o->endpoint);
    free(o->session_id);
    free(o->roundtrip_reasoning_field);
    if (o->extra_headers) {
        for (char **h = o->extra_headers; *h; h++)
            free(*h);
        free(o->extra_headers);
    }
    free(o);
}

void openai_attach_probe(struct provider *p, struct bg_job *probe)
{
    if (!p)
        return;
    struct openai *o = (struct openai *)p;
    o->probe = probe;
}

void openai_context_probe_reset(struct provider *p)
{
    if (!p)
        return;
    struct openai *o = (struct openai *)p;
    /* Settle the in-flight probe before a re-probe so the superseded worker
     * can't land late and overwrite the new model's limit, then drop the
     * limit to unknown. The slot is cleared so the follow-up
     * openai_attach_probe just stores the fresh handle. */
    if (o->probe) {
        bg_job_cancel(o->probe);
        bg_job_join(o->probe);
        o->probe = NULL;
    }
    atomic_store(&o->base.context_limit, 0);
}

/* Duplicate a NULL-terminated array of header strings. Returns NULL when
 * the input is NULL or empty (no headers); otherwise a heap-owned array
 * with each element xstrdup'd, terminated by a NULL slot. */
static char **dup_headers(const char *const *src)
{
    if (!src || !*src)
        return NULL;
    size_t n = 0;
    for (const char *const *p = src; *p; p++)
        n++;
    char **out = xmalloc(sizeof(*out) * (n + 1));
    for (size_t i = 0; i < n; i++)
        out[i] = xstrdup(src[i]);
    out[n] = NULL;
    return out;
}

/* Build the config key for one preset-resolved leaf. A config-defined
 * provider (preset->config_prefix = "providers.<name>") reads its own
 * subtree; the compiled-in shims (prefix NULL) read the shared global
 * "openai.*" namespace. Caller frees. */
static char *preset_key(const char *prefix, const char *leaf)
{
    return xasprintf("%s.%s", prefix ? prefix : "openai", leaf);
}

/* Resolve the reasoning round-trip field. The <prefix>.reasoning_roundtrip
 * setting (HAX_REASONING_ROUNDTRIP for the global namespace), when set,
 * overrides the preset default: "off"/"0"/empty disables; "on"/"1" selects the
 * canonical "reasoning_content"; any other value is used verbatim as the field
 * name (e.g. "reasoning" for routers that key on that). Returns heap-owned or
 * NULL (disabled). */
static char *resolve_roundtrip_field(const char *prefix, const char *preset_default)
{
    char *k = preset_key(prefix, "reasoning_roundtrip");
    const char *env = config_str(k);
    free(k);
    const char *chosen = preset_default;
    if (env) {
        if (!*env || strcmp(env, "off") == 0 || strcmp(env, "0") == 0)
            chosen = NULL;
        else if (strcmp(env, "on") == 0 || strcmp(env, "1") == 0)
            chosen = "reasoning_content";
        else
            chosen = env;
    }
    return chosen ? xstrdup(chosen) : NULL;
}

/* ---------- runtime pickers (model / effort) ---------- */

/* GET <base_url>/models and collect the `data[].id` slugs. The OpenAI
 * /v1/models shape is shared across the whole compat family — real OpenAI,
 * llama.cpp, ollama, OpenRouter — so this one parser serves every shim. */
static int openai_list_models(struct provider *p, char ***ids, size_t *n, char **err,
                              http_tick_cb tick, void *tick_user)
{
    struct openai *o = (struct openai *)p;
    *ids = NULL;
    *n = 0;
    char *url = xasprintf("%s/models", o->base_url);
    char *auth = o->api_key ? xasprintf("Authorization: Bearer %s", o->api_key) : NULL;
    const char *headers[] = {auth, NULL};
    char *body = NULL;
    long status = 0;
    int rc = http_get(url, auth ? headers : NULL, MODEL_LIST_TIMEOUT_S, 0, tick, tick_user, &body,
                      &status);
    free(auth);
    free(url);
    if (rc != 0) {
        *err = format_models_error(p->name, o->base_url, o->api_key != NULL, status);
        free(body);
        return -1;
    }
    json_t *root = json_loads(body, 0, NULL);
    free(body);
    if (!root) {
        *err = xasprintf("%s /models response is not valid JSON", p->name ? p->name : "provider");
        return -1;
    }
    /* An empty "data" array — or ollama's "data": null when nothing is
     * pulled — is a reachable server with a legitimately empty catalog:
     * report zero models so the caller says "no models available". Any
     * other non-array shape (missing field, object, a 200 error payload
     * like {"message":...}) is a malformed catalog, not an empty one. */
    json_t *data = json_object_get(root, "data");
    if (json_is_null(data) || (json_is_array(data) && json_array_size(data) == 0)) {
        json_decref(root);
        return 0;
    }
    const char *name = p->name ? p->name : "provider";
    if (!json_is_array(data)) {
        json_decref(root);
        *err = xasprintf("%s /models response has no model list", name);
        return -1;
    }
    size_t cnt = json_array_size(data);
    char **out = xmalloc(cnt * sizeof(*out));
    size_t k = 0;
    for (size_t i = 0; i < cnt; i++) {
        json_t *id = json_object_get(json_array_get(data, i), "id");
        if (json_is_string(id) && *json_string_value(id))
            out[k++] = xstrdup(json_string_value(id));
    }
    json_decref(root);
    /* Entries existed but none carried a usable id — malformed, not empty. */
    if (k == 0) {
        free(out);
        *err = xasprintf("%s /models response contains no usable model ids", name);
        return -1;
    }
    *ids = out;
    *n = k;
    return 0;
}

static size_t openai_list_efforts(struct provider *p, const char *const **out)
{
    struct openai *o = (struct openai *)p;
    if (!o->efforts || o->n_efforts == 0)
        return 0;
    *out = o->efforts;
    return o->n_efforts;
}

int openai_key_available(const char *api_key_env, const char *miss_reason, const char **reason)
{
    const char *key = config_str("openai.api_key");
    if (key && *key)
        return 1;
    if (api_key_env) {
        const char *e = getenv(api_key_env);
        if (e && *e)
            return 1;
    }
    if (reason)
        *reason = miss_reason ? miss_reason : "no API key";
    return 0;
}

int openai_base_url_reachable(const char *base_url, const char *api_key, const char **reason)
{
    char *url = xasprintf("%s/models", base_url);
    char *auth = (api_key && *api_key) ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *headers[] = {auth, NULL};
    char *body = NULL;
    int rc =
        http_get(url, auth ? headers : NULL, AVAIL_PROBE_TIMEOUT_S, 0, NULL, NULL, &body, NULL);
    free(body);
    free(auth);
    free(url);
    if (rc != 0) {
        if (reason)
            *reason = "server not reachable";
        return 0;
    }
    return 1;
}

static int openai_available(const char *name, const char **reason)
{
    (void)name;
    /* Fixed endpoint (api.openai.com), so selectability is just "is a key
     * configured" — HAX_OPENAI_BASE_URL no longer affects it (the preset
     * locks the base URL and ignores the override). */
    return openai_key_available("OPENAI_API_KEY", "OPENAI_API_KEY not set", reason);
}

struct provider *openai_provider_new_preset(const struct openai_preset *preset)
{
    struct openai_preset zero = {0};
    if (!preset)
        preset = &zero;

    /* HAX_OPENAI_BASE_URL beats the preset default — unless the preset locks
     * its base URL (openai, openrouter: fixed endpoints), in which case the
     * shared override is ignored so a value left set for another backend can't
     * redirect them. One of them must resolve to a non-empty value; if neither
     * does, the calling preset is misconfigured (a programmer error inside
     * hax) — fail loudly so we don't silently default to api.openai.com under
     * a different preset's display name. */
    char *base_key = preset_key(preset->config_prefix, "base_url");
    const char *base_cfg = config_str(base_key);
    free(base_key);
    const char *base =
        (!preset->lock_base_url && base_cfg && *base_cfg) ? base_cfg : preset->default_base_url;
    if (!base || !*base) {
        hax_err("internal: openai preset has no base URL");
        return NULL;
    }
    char *base_url = dup_trim_trailing_slash(base);

    /* Key resolution: <prefix>.api_key (HAX_OPENAI_API_KEY for the global
     * namespace) → preset->api_key_env (e.g. OPENAI_API_KEY for the openai
     * preset, OPENROUTER_API_KEY for openrouter, or a config-defined
     * provider's declared key var). The openai-compatible preset deliberately
     * leaves api_key_env unset so a globally configured OPENAI_API_KEY doesn't
     * leak to a custom endpoint. */
    char *key_key = preset_key(preset->config_prefix, "api_key");
    const char *key = config_str(key_key);
    free(key_key);
    if ((!key || !*key) && preset->api_key_env)
        key = getenv(preset->api_key_env);

    /* Display name: a config-defined provider (config_prefix set) takes its
     * banner from preset->display_name only — the already-resolved
     * providers.<name>.display_name overlaid on its recipe — so a stray
     * global HAX_PROVIDER_NAME can't rename it. The shared shims read the
     * global provider_name override. */
    const char *name = preset->config_prefix ? NULL : config_str("provider_name");
    if (!name || !*name)
        name = (preset->display_name && *preset->display_name) ? preset->display_name : "openai";

    /* prompt_cache_key is an OpenAI-specific affinity hint for prefix-cache
     * routing — but some local servers (notably vLLM) reject unknown JSON
     * fields, hence the per-preset default. <prefix>.send_cache_key
     * (HAX_OPENAI_SEND_CACHE_KEY globally) overrides it in either direction
     * via the shared bool grammar (an explicit false/0/off must not read as
     * "set → on"). */
    char *cache_key_key = preset_key(preset->config_prefix, "send_cache_key");
    int send_cache_key = config_bool_or(cache_key_key, preset->send_cache_key_default);
    free(cache_key_key);

    struct openai *o = xcalloc(1, sizeof(*o));
    o->base_url = base_url;
    o->api_key = (key && *key) ? xstrdup(key) : NULL;
    o->name_buf = xstrdup(name);
    o->endpoint = xasprintf("%s/chat/completions", o->base_url);
    o->send_cache_key = send_cache_key;
    o->emit_progress = preset->emit_progress;
    char *cost_key = preset_key(preset->config_prefix, "request_cost");
    o->request_cost = config_bool_or(cost_key, preset->request_cost);
    free(cost_key);
    o->length_hint = preset->length_hint;
    o->roundtrip_reasoning_field =
        resolve_roundtrip_field(preset->config_prefix, preset->roundtrip_reasoning_field);
    o->reasoning_format = preset->reasoning_format;
    o->efforts = preset->efforts;
    o->n_efforts = preset->n_efforts;
    o->extra_headers = dup_headers(preset->extra_headers);
    char uuid[37];
    gen_uuid_v4(uuid);
    o->session_id = xstrdup(uuid);
    o->base.name = o->name_buf;
    o->base.catalog_id = preset->catalog_id;
    o->base.stream = openai_stream;
    o->base.list_models = openai_list_models;
    o->base.list_efforts = openai_list_efforts;
    o->base.destroy = openai_destroy;
    return &o->base;
}

struct provider *openai_provider_new(const char *name)
{
    (void)name;
    /* Real OpenAI: api.openai.com is fixed. HAX_OPENAI_BASE_URL is ignored
     * (lock_base_url) rather than honored — a custom endpoint belongs on the
     * dedicated "openai-compatible" preset, which keeps OpenAI's policies
     * (OPENAI_API_KEY pickup, prompt_cache_key on) from reaching a third-party
     * host. Ignoring rather than rejecting means a base URL left set for
     * another backend doesn't block selecting openai. */
    struct openai_preset preset = {
        .display_name = "openai",
        .default_base_url = "https://api.openai.com/v1",
        .api_key_env = "OPENAI_API_KEY",
        .send_cache_key_default = 1,
        .lock_base_url = 1,
        .catalog_id = "openai",
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    struct provider *p = openai_provider_new_preset(&preset);
    if (p) {
        /* /v1/models interleaves snapshots, fine-tunes, and aliases in no
         * useful order — alphabetize the picker. */
        p->sort_models = 1;
    }
    return p;
}

const struct provider_factory PROVIDER_OPENAI = {
    .name = "openai",
    .new = openai_provider_new,
    .available = openai_available,
};
