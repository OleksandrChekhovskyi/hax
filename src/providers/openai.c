/* SPDX-License-Identifier: MIT */
#include "openai.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openai_events.h"
#include "util.h"
#include "system/bg_job.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

struct openai {
    struct provider base;
    char *base_url;   /* e.g. "http://127.0.0.1:8000/v1" (no trailing slash) */
    char *api_key;    /* may be NULL for unauthenticated local servers */
    char *name_buf;   /* backing storage for base.name (heap-owned) */
    char *endpoint;   /* base_url + "/chat/completions" */
    char *session_id; /* sent as prompt_cache_key when send_cache_key is set */
    int send_cache_key;
    enum reasoning_format reasoning_format;
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
 * ITEM_ASSISTANT_MESSAGE + ITEM_TOOL_CALL items. OpenAI accepts a single
 * assistant message with both `content` and `tool_calls`; some compat
 * servers reject bare tool_call-only messages without that wrapper.
 *
 * Lossy corner: if a turn produced text → tool_call → text, both text
 * fragments are concatenated into one `content` — Chat Completions has
 * no way to interleave text with tool_calls within a message. Splitting
 * the trailing text into a post-tool-result message would imply it was
 * said after observing the result, which is more misleading. */
static size_t emit_assistant_group(json_t *arr, const struct item *items, size_t i, size_t n)
{
    struct buf text;
    buf_init(&text);
    json_t *tool_calls = NULL;

    while (i < n && (items[i].kind == ITEM_ASSISTANT_MESSAGE || items[i].kind == ITEM_TOOL_CALL)) {
        if (items[i].kind == ITEM_ASSISTANT_MESSAGE) {
            if (items[i].text)
                buf_append_str(&text, items[i].text);
        } else {
            if (!tool_calls)
                tool_calls = json_array();
            json_array_append_new(tool_calls, build_tool_call(&items[i]));
        }
        i++;
    }

    json_t *msg = json_object();
    json_object_set_new(msg, "role", json_string("assistant"));
    if (text.len > 0)
        json_object_set_new(msg, "content", json_string(text.data));
    else
        json_object_set_new(msg, "content", json_null());
    if (tool_calls)
        json_object_set_new(msg, "tool_calls", tool_calls);
    json_array_append_new(arr, msg);

    buf_free(&text);
    return i;
}

static json_t *build_messages(const char *system_prompt, const struct item *items, size_t n)
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
            i = emit_assistant_group(arr, items, i, n);
            break;
        case ITEM_TOOL_RESULT:
            json_array_append_new(arr,
                                  json_pack("{s:s, s:s, s:s}", "role", "tool", "tool_call_id",
                                            items[i].call_id ? items[i].call_id : "", "content",
                                            items[i].output ? items[i].output : ""));
            i++;
            break;
        case ITEM_REASONING:
            /* Codex-only blob; nothing to translate to Chat Completions. */
            i++;
            break;
        case ITEM_TURN_BOUNDARY:
            /* Agent-side marker for the transcript renderer; nothing to
             * translate. emit_assistant_group's while clause already
             * stops at this kind, so it cleanly ends an assistant
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
            fprintf(stderr, "hax: bad tool schema for %s: %s\n", tools[i].name, err.text);
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
    if (strcmp(s, "flat") == 0)
        return REASONING_FLAT;
    if (strcmp(s, "nested") == 0)
        return REASONING_NESTED;
    fprintf(stderr,
            "hax: unknown reasoning format %s (expected 'flat' or 'nested') — using default\n", s);
    return fallback;
}

static char *build_body(const struct context *ctx, const char *model, const char *cache_key,
                        enum reasoning_format reasoning)
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
                             build_messages(ctx->system_prompt, ctx->items, ctx->n_items),
                             "stream_options", "include_usage", 1);

    if (ctx->n_tools > 0)
        json_object_set_new(body, "tools", build_tools(ctx->tools, ctx->n_tools));

    if (cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(cache_key));

    switch (reasoning) {
    case REASONING_FLAT:
        if (ctx->reasoning_effort)
            json_object_set_new(body, "reasoning_effort", json_string(ctx->reasoning_effort));
        break;
    case REASONING_NESTED: {
        /* `enabled: true` is the opt-in some routers need to wake CoT
         * emission on models that otherwise stay silent. Effort
         * piggybacks in the same object when set. */
        json_t *r = json_pack("{s:b}", "enabled", 1);
        if (ctx->reasoning_effort)
            json_object_set_new(r, "effort", json_string(ctx->reasoning_effort));
        json_object_set_new(body, "reasoning", r);
        break;
    }
    }

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

    char *body =
        build_body(ctx, model, o->send_cache_key ? o->session_id : NULL, o->reasoning_format);
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

struct provider *openai_provider_new_preset(const struct openai_preset *preset)
{
    struct openai_preset zero = {0};
    if (!preset)
        preset = &zero;

    /* HAX_OPENAI_BASE_URL beats the preset default; one of them must
     * resolve to a non-empty value. If neither does, the calling preset
     * is misconfigured (a programmer error inside hax) — fail loudly so
     * we don't silently default to api.openai.com under a different
     * preset's display name. */
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    const char *base = (base_env && *base_env) ? base_env : preset->default_base_url;
    if (!base || !*base) {
        fprintf(stderr, "hax: internal: openai preset has no base URL\n");
        return NULL;
    }
    char *base_url = dup_trim_trailing_slash(base);

    /* Key resolution: HAX_OPENAI_API_KEY → preset->api_key_env (e.g.
     * OPENAI_API_KEY for the openai preset, OPENROUTER_API_KEY for
     * openrouter). The openai-compatible preset deliberately leaves
     * api_key_env unset so a globally configured OPENAI_API_KEY doesn't
     * leak to a custom endpoint. */
    const char *key = getenv("HAX_OPENAI_API_KEY");
    if ((!key || !*key) && preset->api_key_env)
        key = getenv(preset->api_key_env);

    const char *name = getenv("HAX_PROVIDER_NAME");
    if (!name || !*name)
        name = (preset->display_name && *preset->display_name) ? preset->display_name : "openai";

    /* prompt_cache_key is an OpenAI-specific affinity hint for prefix-cache
     * routing — backends that don't honor it just ignore the field.
     * HAX_OPENAI_SEND_CACHE_KEY, if set to any non-empty value, forces it
     * on regardless of the preset's default. */
    const char *force_env = getenv("HAX_OPENAI_SEND_CACHE_KEY");
    int send_cache_key = (force_env && *force_env) ? 1 : preset->send_cache_key_default;

    struct openai *o = xcalloc(1, sizeof(*o));
    o->base_url = base_url;
    o->api_key = (key && *key) ? xstrdup(key) : NULL;
    o->name_buf = xstrdup(name);
    o->endpoint = xasprintf("%s/chat/completions", o->base_url);
    o->send_cache_key = send_cache_key;
    o->reasoning_format = preset->reasoning_format;
    o->extra_headers = dup_headers(preset->extra_headers);
    char uuid[37];
    gen_uuid_v4(uuid);
    o->session_id = xstrdup(uuid);
    o->base.name = o->name_buf;
    o->base.stream = openai_stream;
    o->base.destroy = openai_destroy;
    return &o->base;
}

struct provider *openai_provider_new(void)
{
    /* Real OpenAI: api.openai.com is hard-coded and HAX_OPENAI_BASE_URL is
     * rejected. Custom OpenAI-compat endpoints belong on the dedicated
     * "openai-compatible" preset, which keeps the policies that matter
     * for OpenAI itself (OPENAI_API_KEY pickup, prompt_cache_key on)
     * from leaking to a third-party server. */
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    if (base_env && *base_env) {
        fprintf(stderr, "hax: HAX_OPENAI_BASE_URL is not honored by HAX_PROVIDER=openai "
                        "(this preset is locked to api.openai.com)\n"
                        "hax: use HAX_PROVIDER=openai-compatible to point at a custom endpoint\n");
        return NULL;
    }
    struct openai_preset preset = {
        .display_name = "openai",
        .default_base_url = "https://api.openai.com/v1",
        .api_key_env = "OPENAI_API_KEY",
        .send_cache_key_default = 1,
    };
    return openai_provider_new_preset(&preset);
}

const struct provider_factory PROVIDER_OPENAI = {
    .name = "openai",
    .new = openai_provider_new,
};
