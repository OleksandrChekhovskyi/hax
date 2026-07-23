/* SPDX-License-Identifier: MIT */
#include "anthropic.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "anthropic_events.h"
#include "config.h"
#include "util.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

#define ANTHROPIC_DEFAULT_VERSION    "2023-06-01"
#define ANTHROPIC_DEFAULT_MODEL      "claude-opus-4-8"
#define ANTHROPIC_DEFAULT_MAX_TOKENS 32000

/* Short timeout for the /model picker's catalog fetch, mirroring the openai
 * family — tolerant of a small catalog without hanging on a wonky link. */
#define MODEL_LIST_TIMEOUT_S 10

const char *const ANTHROPIC_EFFORT_LADDER[] = {"low", "medium", "high", "xhigh", "max"};
const size_t ANTHROPIC_EFFORT_LADDER_N =
    sizeof(ANTHROPIC_EFFORT_LADDER) / sizeof(ANTHROPIC_EFFORT_LADDER[0]);

struct anthropic {
    struct provider base;
    char *base_url;   /* e.g. "https://api.anthropic.com/v1" (no trailing slash) */
    char *api_key;    /* may be NULL for an unauthenticated local compat server */
    char *name_buf;   /* backing storage for base.name */
    char *endpoint;   /* base_url + "/messages" */
    char *version;    /* anthropic-version header value */
    char *cfg_prefix; /* config namespace: NULL (global anthropic.*) or "providers.<name>" */
    enum anthropic_thinking_mode default_mode;
    int allow_empty_signature;
    int send_cache_control_default;
};

/* Build the config key for one preset-resolved leaf: "<prefix>.<leaf>". A
 * config-defined provider (prefix "providers.<name>") reads its own subtree;
 * the compiled-in shims (prefix NULL) read the shared global "anthropic.*"
 * namespace, env-bound via HAX_ANTHROPIC_*. Caller frees. Mirrors openai.c's
 * preset_key. */
static char *preset_key(const char *prefix, const char *leaf)
{
    return xasprintf("%s.%s", prefix ? prefix : "anthropic", leaf);
}

/* ---------- message translation ---------- */

static json_t *content_text_block(const char *text)
{
    return json_pack("{s:s, s:s}", "type", "text", "text", text ? text : "");
}

static int reasoning_stamp_ok(const struct item *it, const char *cur_provider,
                              const char *cur_model)
{
    return it->provider && it->model && cur_provider && cur_model &&
           strcmp(it->provider, cur_provider) == 0 && strcmp(it->model, cur_model) == 0;
}

/* Append the thinking/redacted_thinking block stored in reasoning_json to the
 * assistant content `blocks`, honoring the empty-signature policy. A thinking
 * block with an empty signature can't be verified by real Anthropic (it 400s),
 * so unless the preset opts into empty signatures it is downgraded to a plain
 * text block (preserving the reasoning as context) or dropped when it has no
 * text either. */
static void append_reasoning_block(json_t *blocks, const struct item *it, int allow_empty_signature)
{
    if (!it->reasoning_json)
        return;
    json_t *obj = json_loads(it->reasoning_json, 0, NULL);
    if (!obj)
        return;
    const char *type = json_string_value(json_object_get(obj, "type"));
    if (type && strcmp(type, "thinking") == 0 && !allow_empty_signature) {
        const char *sig = json_string_value(json_object_get(obj, "signature"));
        if (!sig || !*sig) {
            const char *think = json_string_value(json_object_get(obj, "thinking"));
            if (think && *think)
                json_array_append_new(blocks, content_text_block(think));
            json_decref(obj);
            return;
        }
    }
    json_array_append_new(blocks, obj);
}

/* Emit one assistant message for a run of REASONING + ASSISTANT_MESSAGE +
 * TOOL_CALL items, as an ordered content-block array: thinking block(s) first
 * (so a tool-use turn leads with its preserved reasoning), then text, then
 * tool_use. An empty group (e.g. reasoning-only with a dropped empty-sig
 * block) produces no message. Returns the index past the consumed run. */
static size_t emit_assistant_group(json_t *arr, const struct item *items, size_t i, size_t n,
                                   const char *cur_provider, const char *cur_model,
                                   int allow_empty_signature)
{
    json_t *blocks = json_array();
    while (i < n && (items[i].kind == ITEM_ASSISTANT_MESSAGE || items[i].kind == ITEM_TOOL_CALL ||
                     items[i].kind == ITEM_REASONING)) {
        const struct item *it = &items[i];
        if (it->kind == ITEM_ASSISTANT_MESSAGE) {
            if (it->text && *it->text)
                json_array_append_new(blocks, content_text_block(it->text));
        } else if (it->kind == ITEM_REASONING) {
            if (reasoning_stamp_ok(it, cur_provider, cur_model))
                append_reasoning_block(blocks, it, allow_empty_signature);
        } else { /* ITEM_TOOL_CALL */
            json_t *input =
                it->tool_arguments_json ? json_loads(it->tool_arguments_json, 0, NULL) : NULL;
            if (!json_is_object(input)) {
                if (input)
                    json_decref(input);
                input = json_object();
            }
            json_array_append_new(blocks,
                                  json_pack("{s:s, s:s, s:s, s:o}", "type", "tool_use", "id",
                                            it->call_id ? it->call_id : "", "name",
                                            it->tool_name ? it->tool_name : "", "input", input));
        }
        i++;
    }
    if (json_array_size(blocks) == 0)
        json_decref(blocks);
    else
        json_array_append_new(arr, json_pack("{s:s, s:o}", "role", "assistant", "content", blocks));
    return i;
}

/* Coalesce a run of consecutive TOOL_RESULT items into a single user message
 * carrying tool_result blocks — the Messages API groups results that way, and
 * some strict compat endpoints reject a tool_result message split per call.
 * A result carrying image parts switches its content from the plain string
 * to a block array; image_input == 0 degrades each part to a text
 * placeholder block. */
static size_t emit_tool_results(json_t *arr, const struct item *items, size_t i, size_t n,
                                int image_input)
{
    json_t *content = json_array();
    while (i < n && items[i].kind == ITEM_TOOL_RESULT) {
        const struct item *it = &items[i];
        if (it->n_images == 0) {
            json_array_append_new(content, json_pack("{s:s, s:s, s:s}", "type", "tool_result",
                                                     "tool_use_id", it->call_id ? it->call_id : "",
                                                     "content", it->output ? it->output : ""));
        } else {
            json_t *blocks = json_array();
            if (it->output && *it->output)
                json_array_append_new(blocks, content_text_block(it->output));
            for (size_t k = 0; k < it->n_images; k++) {
                const struct item_image *img = &it->images[k];
                if (image_input != 0) {
                    json_array_append_new(blocks,
                                          json_pack("{s:s, s:{s:s, s:s, s:s}}", "type", "image",
                                                    "source", "type", "base64", "media_type",
                                                    img->mime ? img->mime : "image/png", "data",
                                                    img->data_b64 ? img->data_b64 : ""));
                } else {
                    char *ph = item_image_placeholder(img);
                    json_array_append_new(blocks, content_text_block(ph));
                    free(ph);
                }
            }
            json_array_append_new(content,
                                  json_pack("{s:s, s:s, s:o}", "type", "tool_result", "tool_use_id",
                                            it->call_id ? it->call_id : "", "content", blocks));
        }
        i++;
    }
    json_array_append_new(arr, json_pack("{s:s, s:o}", "role", "user", "content", content));
    return i;
}

json_t *anthropic_build_messages(const struct item *items, size_t n, const char *cur_provider,
                                 const char *cur_model, int allow_empty_signature, int image_input)
{
    json_t *arr = json_array();
    size_t i = 0;
    while (i < n) {
        switch (items[i].kind) {
        case ITEM_USER_MESSAGE: {
            json_t *blocks = json_array();
            json_array_append_new(blocks, content_text_block(items[i].text));
            json_array_append_new(arr, json_pack("{s:s, s:o}", "role", "user", "content", blocks));
            i++;
            break;
        }
        case ITEM_ASSISTANT_MESSAGE:
        case ITEM_TOOL_CALL:
        case ITEM_REASONING:
            /* Reasoning leads an assistant group (thinking precedes text/tool
             * use); a bare assistant message or tool call starts one too. */
            i = emit_assistant_group(arr, items, i, n, cur_provider, cur_model,
                                     allow_empty_signature);
            break;
        case ITEM_TOOL_RESULT:
            i = emit_tool_results(arr, items, i, n, image_input);
            break;
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            i++;
            break;
        }
    }
    return arr;
}

/* ---------- request body ---------- */

static json_t *make_cache_control(const char *ttl)
{
    json_t *cc = json_pack("{s:s}", "type", "ephemeral");
    if (ttl && strcasecmp(ttl, "1h") == 0)
        json_object_set_new(cc, "ttl", json_string("1h"));
    return cc;
}

static json_t *build_tools(const struct tool_def *tools, size_t n, int cache_last, const char *ttl)
{
    json_t *arr = json_array();
    for (size_t i = 0; i < n; i++) {
        json_error_t err;
        json_t *schema = json_loads(tools[i].parameters_schema_json, 0, &err);
        if (!schema) {
            hax_warn("bad tool schema for %s: %s", tools[i].name, err.text);
            schema = json_object();
        }
        json_t *tool = json_pack("{s:s, s:s, s:o}", "name", tools[i].name, "description",
                                 tools[i].description, "input_schema", schema);
        if (cache_last && i == n - 1)
            json_object_set_new(tool, "cache_control", make_cache_control(ttl));
        json_array_append_new(arr, tool);
    }
    return arr;
}

/* Place a cache breakpoint on the last content block of the last message — the
 * rolling tail of the conversation, so the whole prompt prefix is cached for
 * the next request. Skips a trailing thinking block (cache_control belongs on
 * text/tool_use/tool_result, never thinking). Combined with the system and
 * last-tool breakpoints this stays at 3, under Anthropic's cap of 4. */
static void attach_cache_to_last_message(json_t *messages, const char *ttl)
{
    size_t n = json_array_size(messages);
    if (n == 0)
        return;
    json_t *content = json_object_get(json_array_get(messages, n - 1), "content");
    if (!json_is_array(content) || json_array_size(content) == 0)
        return;
    json_t *block = json_array_get(content, json_array_size(content) - 1);
    const char *bt = json_string_value(json_object_get(block, "type"));
    /* cache_control belongs on text / tool_use / tool_result, never on a
     * thinking or redacted_thinking block. */
    if (bt && (strcmp(bt, "thinking") == 0 || strcmp(bt, "redacted_thinking") == 0))
        return;
    json_object_set_new(block, "cache_control", make_cache_control(ttl));
}

static enum anthropic_thinking_mode resolve_thinking_mode(struct anthropic *a)
{
    char *k = preset_key(a->cfg_prefix, "thinking_mode");
    const char *m = config_str_nonempty(k);
    free(k);
    if (m) {
        if (strcasecmp(m, "adaptive") == 0)
            return ANTHROPIC_THINKING_ADAPTIVE;
        if (strcasecmp(m, "budget") == 0)
            return ANTHROPIC_THINKING_BUDGET;
        if (strcasecmp(m, "off") == 0)
            return ANTHROPIC_THINKING_OFF;
        hax_warn("unknown anthropic.thinking_mode '%s' (adaptive/budget/off) — using default", m);
    }
    return a->default_mode;
}

/* Configure the thinking parameter. Adaptive mode (real flagships) sends
 * thinking:{type:"adaptive"} plus the categorical output_config.effort, with
 * display following show_reasoning (summarized when the user wants live CoT,
 * omitted otherwise for faster time-to-first-text — the signature still
 * round-trips either way). Budget mode (older / local compat) sends
 * thinking:{type:"enabled", budget_tokens}; the budget fits inside max_tokens
 * (Anthropic requires budget < max_tokens), defaulting to "fill the window". */
static void apply_thinking(json_t *body, struct anthropic *a, const struct context *ctx,
                           int max_tokens)
{
    enum anthropic_thinking_mode mode = resolve_thinking_mode(a);
    if (mode == ANTHROPIC_THINKING_OFF)
        return;

    if (mode == ANTHROPIC_THINKING_ADAPTIVE) {
        const char *display = config_bool("show_reasoning") ? "summarized" : "omitted";
        json_object_set_new(body, "thinking",
                            json_pack("{s:s, s:s}", "type", "adaptive", "display", display));
        if (ctx->effort && *ctx->effort)
            json_object_set_new(body, "output_config", json_pack("{s:s}", "effort", ctx->effort));
        return;
    }

    /* BUDGET. Anthropic requires 1 <= budget_tokens < max_tokens, so a window
     * under two tokens can't hold any thinking — leave it off rather than emit
     * an out-of-range budget (max_tokens is a plain positive count here, not
     * necessarily the registry-bounded one: a config-defined provider's
     * providers.<name>.max_tokens carries no bound). */
    if (max_tokens < 2)
        return;
    char *k = preset_key(a->cfg_prefix, "thinking_budget");
    int budget = config_int(k);
    free(k);
    if (budget <= 0 || budget >= max_tokens)
        budget = max_tokens - 1;
    json_object_set_new(body, "thinking",
                        json_pack("{s:s, s:i}", "type", "enabled", "budget_tokens", budget));
}

static char *build_body(struct anthropic *a, const struct context *ctx, const char *model)
{
    char *k = preset_key(a->cfg_prefix, "max_tokens");
    int max_tokens = config_int(k);
    free(k);
    if (max_tokens <= 0)
        max_tokens = ANTHROPIC_DEFAULT_MAX_TOKENS;

    k = preset_key(a->cfg_prefix, "cache");
    int cache = config_bool_or(k, a->send_cache_control_default);
    free(k);
    k = preset_key(a->cfg_prefix, "cache_ttl");
    const char *ttl = config_str_nonempty(k);
    free(k);

    json_t *messages = anthropic_build_messages(ctx->items, ctx->n_items, a->base.name, model,
                                                a->allow_empty_signature, ctx->image_input);
    json_t *body = json_pack("{s:s, s:i, s:b, s:o}", "model", model, "max_tokens", max_tokens,
                             "stream", 1, "messages", messages);

    if (ctx->system_prompt && *ctx->system_prompt) {
        json_t *block = content_text_block(ctx->system_prompt);
        if (cache)
            json_object_set_new(block, "cache_control", make_cache_control(ttl));
        json_t *system = json_array();
        json_array_append_new(system, block);
        json_object_set_new(body, "system", system);
    }

    if (ctx->n_tools > 0)
        json_object_set_new(body, "tools", build_tools(ctx->tools, ctx->n_tools, cache, ttl));

    if (cache)
        attach_cache_to_last_message(messages, ttl);

    apply_thinking(body, a, ctx, max_tokens);

    char *s = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return s;
}

/* ---------- SSE glue ---------- */

static int on_sse(const char *event_name, const char *data, void *user)
{
    anthropic_events_feed(user, event_name, data);
    return 0;
}

/* ---------- provider interface ---------- */

static int anthropic_stream(struct provider *p, const struct context *ctx, const char *model,
                            stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    struct anthropic *a = (struct anthropic *)p;

    char *body = build_body(a, ctx, model);
    if (!body)
        return -1;
    size_t body_len = strlen(body);

    char *key_hdr = a->api_key ? xasprintf("x-api-key: %s", a->api_key) : NULL;
    char *ver_hdr = xasprintf("anthropic-version: %s", a->version);
    const char *headers[6];
    size_t hi = 0;
    if (key_hdr)
        headers[hi++] = key_hdr;
    headers[hi++] = ver_hdr;
    headers[hi++] = "Accept: text/event-stream";
    headers[hi++] = "Content-Type: application/json";
    headers[hi] = NULL;

    struct retry_policy pol = retry_policy_default();
    struct http_response resp;
    struct anthropic_events ev;
    int rc = -1;

    for (int attempt = 0; attempt < pol.max_attempts; attempt++) {
        memset(&resp, 0, sizeof(resp));
        anthropic_events_init(&ev, cb, user);
        rc = http_sse_post(a->endpoint, headers, body, body_len, on_sse, &ev, tick, tick_user,
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
        anthropic_events_free(&ev);

        if (retry_sleep_with_tick(delay, tick, tick_user)) {
            resp.cancelled = 1;
            memset(&ev, 0, sizeof(ev));
            break;
        }
    }

    if (resp.cancelled) {
        /* User-initiated abort — agent handles the partial state. */
    } else if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        char *msg = format_api_error(resp.status, resp.error_body);
        struct stream_event e = {
            .kind = EV_ERROR,
            .u.error = {.message = msg, .http_status = (int)resp.status},
        };
        cb(&e, user);
        free(msg);
    } else {
        anthropic_events_finalize(&ev);
    }

    free(resp.error_body);
    free(key_hdr);
    free(ver_hdr);
    free(body);
    anthropic_events_free(&ev);
    return rc;
}

/* GET <base_url>/models and collect data[].id. Anthropic's catalog shares the
 * OpenAI-style {"data":[{"id":...}]} shape, as does llama-server's compat
 * endpoint, so one parser serves both shims. */
static int anthropic_list_models(struct provider *p, char ***ids, size_t *n, char **err,
                                 http_tick_cb tick, void *tick_user)
{
    struct anthropic *a = (struct anthropic *)p;
    *ids = NULL;
    *n = 0;
    char *url = xasprintf("%s/models", a->base_url);
    char *key_hdr = a->api_key ? xasprintf("x-api-key: %s", a->api_key) : NULL;
    char *ver_hdr = xasprintf("anthropic-version: %s", a->version);
    const char *headers[3];
    size_t hi = 0;
    if (key_hdr)
        headers[hi++] = key_hdr;
    headers[hi++] = ver_hdr;
    headers[hi] = NULL;
    char *body = NULL;
    long status = 0;
    int rc = http_get(url, headers, MODEL_LIST_TIMEOUT_S, 0, tick, tick_user, &body, &status);
    free(key_hdr);
    free(ver_hdr);
    free(url);
    if (rc != 0) {
        *err = format_models_error(p->name, a->base_url, a->api_key != NULL, status);
        free(body);
        return -1;
    }
    json_t *root = json_loads(body, 0, NULL);
    free(body);
    if (!root) {
        *err = xasprintf("%s /models response is not valid JSON", p->name ? p->name : "provider");
        return -1;
    }
    /* Same shape policy as openai_list_models: null / empty array = a
     * legitimately empty catalog; any other non-array shape, or entries
     * with no usable ids, = a malformed one. */
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
    if (k == 0) {
        free(out);
        *err = xasprintf("%s /models response contains no usable model ids", name);
        return -1;
    }
    *ids = out;
    *n = k;
    return 0;
}

static size_t anthropic_list_efforts(struct provider *p, const char *const **out)
{
    /* Effort only reaches the wire in adaptive mode (output_config.effort);
     * budget mode ignores it. So advertise the ladder for /effort only when
     * the resolved mode is adaptive — otherwise the picker would persist a
     * setting that has no effect on this provider's requests. */
    struct anthropic *a = (struct anthropic *)p;
    if (resolve_thinking_mode(a) != ANTHROPIC_THINKING_ADAPTIVE)
        return 0;
    *out = ANTHROPIC_EFFORT_LADDER;
    return ANTHROPIC_EFFORT_LADDER_N;
}

static void anthropic_destroy(struct provider *p)
{
    struct anthropic *a = (struct anthropic *)p;
    free(a->base_url);
    free(a->api_key);
    free(a->name_buf);
    free(a->endpoint);
    free(a->version);
    free(a->cfg_prefix);
    free(a);
}

/* ---------- construction ---------- */

struct provider *anthropic_provider_new_preset(const struct anthropic_preset *preset)
{
    struct anthropic_preset zero = {0};
    if (!preset)
        preset = &zero;

    /* Resolve every per-provider setting through preset_key, which reads the
     * preset's own subtree (providers.<name>.*) for a config-defined provider
     * and the shared global "anthropic.*" for the compiled-in shims (NULL
     * prefix). Mirrors openai_provider_new_preset. */
    const char *prefix = preset->config_prefix;

    char *base_key = preset_key(prefix, "base_url");
    const char *base_cfg = config_str(base_key);
    free(base_key);
    const char *base =
        (!preset->lock_base_url && base_cfg && *base_cfg) ? base_cfg : preset->default_base_url;
    if (!base || !*base) {
        hax_err("internal: anthropic preset has no base URL");
        return NULL;
    }

    char *key_key = preset_key(prefix, "api_key");
    const char *key = config_str(key_key);
    free(key_key);
    if ((!key || !*key) && preset->api_key_env)
        key = getenv(preset->api_key_env);

    /* A config-defined provider takes its banner from preset->display_name
     * only (the resolved providers.<name>.display_name); the compiled-in shims
     * read the global provider_name override. */
    const char *name = prefix ? NULL : config_str("provider_name");
    if (!name || !*name)
        name = (preset->display_name && *preset->display_name) ? preset->display_name : "anthropic";

    char *ver_key = preset_key(prefix, "version");
    const char *version = config_str_nonempty(ver_key);
    free(ver_key);
    if (!version)
        version = ANTHROPIC_DEFAULT_VERSION;

    struct anthropic *a = xcalloc(1, sizeof(*a));
    a->base_url = dup_trim_trailing_slash(base);
    a->api_key = (key && *key) ? xstrdup(key) : NULL;
    a->name_buf = xstrdup(name);
    a->endpoint = xasprintf("%s/messages", a->base_url);
    a->version = xstrdup(version);
    a->cfg_prefix = prefix ? xstrdup(prefix) : NULL;
    a->default_mode = preset->default_thinking_mode;
    a->allow_empty_signature = preset->allow_empty_signature;
    a->send_cache_control_default = preset->send_cache_control_default;

    a->base.name = a->name_buf;
    a->base.catalog_id = preset->catalog_id;
    /* Real Anthropic has a fixed default model; the compat shim and
     * config-defined providers leave this NULL (rely on HAX_MODEL or /model). */
    a->base.default_model = preset->lock_base_url ? ANTHROPIC_DEFAULT_MODEL : NULL;
    a->base.stream = anthropic_stream;
    a->base.list_models = anthropic_list_models;
    a->base.list_efforts = anthropic_list_efforts;
    a->base.destroy = anthropic_destroy;
    return &a->base;
}

struct provider *anthropic_provider_new(const char *name)
{
    (void)name;
    struct anthropic_preset preset = {
        .display_name = "anthropic",
        .default_base_url = "https://api.anthropic.com/v1",
        .api_key_env = "ANTHROPIC_API_KEY",
        .lock_base_url = 1,
        .default_thinking_mode = ANTHROPIC_THINKING_ADAPTIVE,
        .allow_empty_signature = 0,
        .send_cache_control_default = 1,
        .catalog_id = "anthropic",
    };
    return anthropic_provider_new_preset(&preset);
}

static int anthropic_key_available(const char *api_key_env, const char **reason)
{
    const char *key = config_str("anthropic.api_key");
    if (key && *key)
        return 1;
    if (api_key_env) {
        const char *e = getenv(api_key_env);
        if (e && *e)
            return 1;
    }
    if (reason)
        *reason = "ANTHROPIC_API_KEY not set";
    return 0;
}

static int anthropic_available(const char *name, const char **reason)
{
    (void)name;
    return anthropic_key_available("ANTHROPIC_API_KEY", reason);
}

const struct provider_factory PROVIDER_ANTHROPIC = {
    .name = "anthropic",
    .new = anthropic_provider_new,
    .available = anthropic_available,
};
