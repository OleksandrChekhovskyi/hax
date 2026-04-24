/* SPDX-License-Identifier: MIT */
#include "codex.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codex_events.h"
#include "http.h"
#include "util.h"

#define CODEX_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"

struct codex {
    struct provider base;
    char *access_token;
    char *account_id;
};

/* ---------- request body construction ---------- */

static json_t *build_input_items(const struct item *items, size_t n)
{
    json_t *arr = json_array();
    for (size_t i = 0; i < n; i++) {
        const struct item *it = &items[i];
        json_t *obj = NULL;

        switch (it->kind) {
        case ITEM_USER_MESSAGE: {
            json_t *content = json_array();
            json_array_append_new(content, json_pack("{s:s, s:s}", "type", "input_text", "text",
                                                     it->text ? it->text : ""));
            obj =
                json_pack("{s:s, s:s, s:o}", "type", "message", "role", "user", "content", content);
            break;
        }
        case ITEM_ASSISTANT_MESSAGE: {
            json_t *content = json_array();
            json_array_append_new(content, json_pack("{s:s, s:s}", "type", "output_text", "text",
                                                     it->text ? it->text : ""));
            obj = json_pack("{s:s, s:s, s:o}", "type", "message", "role", "assistant", "content",
                            content);
            break;
        }
        case ITEM_TOOL_CALL:
            obj = json_pack("{s:s, s:s, s:s, s:s}", "type", "function_call", "call_id",
                            it->call_id ? it->call_id : "", "name",
                            it->tool_name ? it->tool_name : "", "arguments",
                            it->tool_arguments_json ? it->tool_arguments_json : "{}");
            break;
        case ITEM_TOOL_RESULT:
            obj = json_pack("{s:s, s:s, s:s}", "type", "function_call_output", "call_id",
                            it->call_id ? it->call_id : "", "output", it->output ? it->output : "");
            break;
        }
        if (obj)
            json_array_append_new(arr, obj);
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
        json_array_append_new(arr, json_pack("{s:s, s:s, s:s, s:o}", "type", "function", "name",
                                             tools[i].name, "description", tools[i].description,
                                             "parameters", params));
    }
    return arr;
}

static char *build_body(const struct context *ctx, const char *model)
{
    json_t *body = json_pack(
        "{s:s, s:b, s:b, s:s, s:o, s:{s:s}, s:s, s:b, s:o}", "model", model, "store", 0, "stream",
        1, "instructions", ctx->system_prompt ? ctx->system_prompt : "", "input",
        build_input_items(ctx->items, ctx->n_items), "text", "verbosity", "medium", "tool_choice",
        "auto", "parallel_tool_calls", 1, "tools", build_tools(ctx->tools, ctx->n_tools));

    char *s = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return s;
}

/* ---------- SSE glue ---------- */

static int on_sse(const char *event_name, const char *data, void *user)
{
    (void)event_name; /* Codex mirrors the type in the data JSON */
    codex_events_feed(user, data);
    return 0;
}

/* ---------- provider interface ---------- */

static int codex_stream(struct provider *p, const struct context *ctx, const char *model,
                        stream_cb cb, void *user)
{
    struct codex *c = (struct codex *)p;

    char *body = build_body(ctx, model);
    if (!body)
        return -1;
    size_t body_len = strlen(body);

    char *auth_hdr = xasprintf("Authorization: Bearer %s", c->access_token);
    char *acct_hdr = xasprintf("chatgpt-account-id: %s", c->account_id);
#if defined(__APPLE__)
    const char *ua = "User-Agent: hax/0.1 (macos)";
#elif defined(__linux__)
    const char *ua = "User-Agent: hax/0.1 (linux)";
#else
    const char *ua = "User-Agent: hax/0.1";
#endif

    const char *headers[] = {
        auth_hdr,
        acct_hdr,
        "originator: hax",
        ua,
        "OpenAI-Beta: responses=experimental",
        "Accept: text/event-stream",
        "Content-Type: application/json",
        NULL,
    };

    struct codex_events ev;
    codex_events_init(&ev, cb, user);
    struct http_response resp;
    int rc = http_sse_post(CODEX_ENDPOINT, headers, body, body_len, on_sse, &ev, &resp);

    if (resp.status == 401) {
        struct stream_event e = {
            .kind = EV_ERROR,
            .u.error = {.message = "codex token expired — run `codex` "
                                   "once to refresh, then retry",
                        .http_status = 401},
        };
        cb(&e, user);
    } else if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        struct stream_event e = {
            .kind = EV_ERROR,
            .u.error = {.message = resp.error_body ? resp.error_body : "request failed",
                        .http_status = (int)resp.status},
        };
        cb(&e, user);
    } else {
        codex_events_finalize(&ev);
    }

    free(resp.error_body);
    free(auth_hdr);
    free(acct_hdr);
    free(body);
    codex_events_free(&ev);
    return rc;
}

static void codex_destroy(struct provider *p)
{
    struct codex *c = (struct codex *)p;
    free(c->access_token);
    free(c->account_id);
    free(c);
}

struct provider *codex_provider_new(void)
{
    char *path = expand_home("~/.codex/auth.json");
    size_t len = 0;
    char *contents = slurp_file(path, &len);
    if (!contents) {
        fprintf(stderr, "hax: cannot read %s — is the codex CLI installed and logged in?\n", path);
        free(path);
        return NULL;
    }
    free(path);

    json_error_t err;
    json_t *root = json_loads(contents, 0, &err);
    free(contents);
    if (!root) {
        fprintf(stderr, "hax: ~/.codex/auth.json is not valid JSON: %s\n", err.text);
        return NULL;
    }

    json_t *tokens = json_object_get(root, "tokens");
    const char *access = tokens ? json_string_value(json_object_get(tokens, "access_token")) : NULL;
    const char *account = tokens ? json_string_value(json_object_get(tokens, "account_id")) : NULL;
    if (!access || !account) {
        fprintf(stderr, "hax: auth.json missing tokens.access_token or tokens.account_id\n");
        json_decref(root);
        return NULL;
    }

    struct codex *c = xcalloc(1, sizeof(*c));
    c->base.name = "codex";
    c->base.default_model = "gpt-5.3-codex";
    c->base.stream = codex_stream;
    c->base.destroy = codex_destroy;
    c->access_token = xstrdup(access);
    c->account_id = xstrdup(account);

    json_decref(root);
    return &c->base;
}
