/* SPDX-License-Identifier: MIT */
#include "openai.h"

#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"
#include "openai_events.h"
#include "util.h"

struct openai {
    struct provider base;
    char *base_url;   /* e.g. "http://127.0.0.1:8000/v1" (no trailing slash) */
    char *api_key;    /* may be NULL for unauthenticated local servers */
    char *name_buf;   /* backing storage for base.name (heap-owned) */
    char *endpoint;   /* base_url + "/chat/completions" */
    char *session_id; /* sent as prompt_cache_key when send_cache_key is set */
    int send_cache_key;
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

static char *build_body(const struct context *ctx, const char *model, const char *cache_key)
{
    /* Deliberately omit `tool_choice` ("auto" is the backend default when
     * tools are present) and `parallel_tool_calls` (defaults to true on
     * real OpenAI). Sending the field explicitly would risk 400s on
     * strict compat backends that reject unknown JSON, with no upside on
     * real OpenAI. The agent loop already handles multiple tool_calls
     * per turn when the model emits them. */
    json_t *body = json_pack("{s:s, s:b, s:o}", "model", model, "stream", 1, "messages",
                             build_messages(ctx->system_prompt, ctx->items, ctx->n_items));

    if (ctx->n_tools > 0)
        json_object_set_new(body, "tools", build_tools(ctx->tools, ctx->n_tools));

    if (cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(cache_key));

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
                         stream_cb cb, void *user)
{
    struct openai *o = (struct openai *)p;

    char *body = build_body(ctx, model, o->send_cache_key ? o->session_id : NULL);
    if (!body)
        return -1;
    size_t body_len = strlen(body);

    char *auth_hdr = o->api_key ? xasprintf("Authorization: Bearer %s", o->api_key) : NULL;

    const char *headers_auth[] = {
        auth_hdr,
        "Accept: text/event-stream",
        "Content-Type: application/json",
        NULL,
    };
    const char *headers_noauth[] = {
        "Accept: text/event-stream",
        "Content-Type: application/json",
        NULL,
    };
    const char *const *headers = auth_hdr ? headers_auth : headers_noauth;

    struct openai_events ev;
    openai_events_init(&ev, cb, user);
    struct http_response resp;
    int rc = http_sse_post(o->endpoint, headers, body, body_len, on_sse, &ev, &resp);

    if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        struct stream_event e = {
            .kind = EV_ERROR,
            .u.error = {.message = resp.error_body ? resp.error_body : "request failed",
                        .http_status = (int)resp.status},
        };
        cb(&e, user);
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
    free(o->base_url);
    free(o->api_key);
    free(o->name_buf);
    free(o->endpoint);
    free(o->session_id);
    free(o);
}

/* Strip trailing '/' from a base URL so "http://x/v1/" and "http://x/v1"
 * produce the same endpoint. */
static char *dup_trim_trailing_slash(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/')
        n--;
    char *out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* True when the URL targets the real OpenAI host. Used to gate behavior
 * that's only safe/useful against api.openai.com (the OPENAI_API_KEY
 * fallback and the default-on prompt_cache_key). Delegates to libcurl's
 * URL parser so userinfo tricks like "https://api.openai.com:443@attacker/"
 * are correctly rejected — CURLUPART_HOST returns the real host that
 * libcurl will connect to, not whatever sat before the '@'. */
static int is_real_openai_url(const char *url)
{
    CURLU *u = curl_url();
    if (!u)
        return 0;

    int ok = 0;
    char *scheme = NULL;
    char *host = NULL;

    if (curl_url_set(u, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK) {
        ok = strcasecmp(scheme, "https") == 0 && strcasecmp(host, "api.openai.com") == 0;
    }

    curl_free(scheme);
    curl_free(host);
    curl_url_cleanup(u);
    return ok;
}

struct provider *openai_provider_new(void)
{
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    const char *base = (base_env && *base_env) ? base_env : "https://api.openai.com/v1";
    char *base_url = dup_trim_trailing_slash(base);
    int is_real_openai = is_real_openai_url(base_url);

    /* Only fall back to OPENAI_API_KEY when we're hitting real OpenAI —
     * otherwise we'd leak a globally configured OpenAI key to whatever
     * local or third-party server the user pointed us at. */
    const char *key = getenv("HAX_OPENAI_API_KEY");
    if ((!key || !*key) && is_real_openai)
        key = getenv("OPENAI_API_KEY");

    const char *name = getenv("HAX_PROVIDER_NAME");
    if (!name || !*name)
        name = "openai";

    /* Send prompt_cache_key by default to real OpenAI (its prefix-cache
     * routing benefits from a stable affinity hint). For non-OpenAI base
     * URLs the field is risky — vLLM hard-rejects unknown JSON fields —
     * so default off, but allow opt-in for hosted OpenAI-compatible
     * providers (Together, Fireworks, OpenRouter, Groq, ...) that do
     * benefit from it. */
    const char *force_env = getenv("HAX_OPENAI_SEND_CACHE_KEY");
    int force_send = force_env && *force_env;

    struct openai *o = xcalloc(1, sizeof(*o));
    o->base_url = base_url;
    o->api_key = (key && *key) ? xstrdup(key) : NULL;
    o->name_buf = xstrdup(name);
    o->endpoint = xasprintf("%s/chat/completions", o->base_url);
    o->send_cache_key = is_real_openai || force_send;
    char uuid[37];
    gen_uuid_v4(uuid);
    o->session_id = xstrdup(uuid);
    o->base.name = o->name_buf;
    o->base.stream = openai_stream;
    o->base.destroy = openai_destroy;
    return &o->base;
}
