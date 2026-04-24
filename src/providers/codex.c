/* SPDX-License-Identifier: MIT */
#include "codex.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "util.h"

#define CODEX_ENDPOINT "https://chatgpt.com/backend-api/codex/responses"

struct codex {
    struct provider base;
    char *access_token;
    char *account_id;
};

/* Per-stream state — tracks in-flight tool calls so that deltas keyed by
 * item_id can be routed to the right call_id. */
struct tool_track {
    char *item_id;
    char *call_id;
    char *name;
};

struct stream_state {
    stream_cb user_cb;
    void *user;

    struct tool_track *tools;
    size_t n_tools;
    size_t cap_tools;

    int terminated; /* any terminal event (done / error) emitted */
};

static struct tool_track *track_find(struct stream_state *s, const char *item_id)
{
    if (!item_id)
        return NULL;
    for (size_t i = 0; i < s->n_tools; i++) {
        if (s->tools[i].item_id && strcmp(s->tools[i].item_id, item_id) == 0)
            return &s->tools[i];
    }
    return NULL;
}

static struct tool_track *track_add(struct stream_state *s, const char *item_id,
                                    const char *call_id, const char *name)
{
    if (s->n_tools == s->cap_tools) {
        size_t cap = s->cap_tools ? s->cap_tools * 2 : 4;
        s->tools = xrealloc(s->tools, cap * sizeof(*s->tools));
        s->cap_tools = cap;
    }
    struct tool_track *t = &s->tools[s->n_tools++];
    t->item_id = xstrdup(item_id);
    t->call_id = xstrdup(call_id);
    t->name = xstrdup(name);
    return t;
}

static void tracks_free(struct stream_state *s)
{
    for (size_t i = 0; i < s->n_tools; i++) {
        free(s->tools[i].item_id);
        free(s->tools[i].call_id);
        free(s->tools[i].name);
    }
    free(s->tools);
    s->tools = NULL;
    s->n_tools = s->cap_tools = 0;
}

static int emit(struct stream_state *s, const struct stream_event *ev)
{
    return s->user_cb(ev, s->user);
}

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

/* ---------- SSE event → stream_event translation ---------- */

static void handle_output_item_added(struct stream_state *s, json_t *root)
{
    json_t *item = json_object_get(root, "item");
    if (!item)
        return;
    const char *type = json_string_value(json_object_get(item, "type"));
    if (!type)
        return;

    if (strcmp(type, "function_call") == 0) {
        const char *item_id = json_string_value(json_object_get(item, "id"));
        const char *call_id = json_string_value(json_object_get(item, "call_id"));
        const char *name = json_string_value(json_object_get(item, "name"));
        if (!item_id || !call_id || !name)
            return;
        track_add(s, item_id, call_id, name);

        struct stream_event ev = {
            .kind = EV_TOOL_CALL_START,
            .u.tool_call_start = {.id = call_id, .name = name},
        };
        emit(s, &ev);
    }
    /* "message" items need no start event; text deltas imply open. */
}

static void handle_output_item_done(struct stream_state *s, json_t *root)
{
    json_t *item = json_object_get(root, "item");
    if (!item)
        return;
    const char *type = json_string_value(json_object_get(item, "type"));
    if (!type)
        return;
    if (strcmp(type, "function_call") != 0)
        return;

    const char *item_id = json_string_value(json_object_get(item, "id"));
    struct tool_track *t = track_find(s, item_id);
    if (!t)
        return;

    struct stream_event ev = {
        .kind = EV_TOOL_CALL_END,
        .u.tool_call_end = {.id = t->call_id},
    };
    emit(s, &ev);
}

static void handle_text_delta(struct stream_state *s, json_t *root)
{
    const char *delta = json_string_value(json_object_get(root, "delta"));
    if (!delta)
        return;
    struct stream_event ev = {
        .kind = EV_TEXT_DELTA,
        .u.text_delta = {.text = delta},
    };
    emit(s, &ev);
}

static void handle_args_delta(struct stream_state *s, json_t *root)
{
    const char *item_id = json_string_value(json_object_get(root, "item_id"));
    const char *delta = json_string_value(json_object_get(root, "delta"));
    if (!item_id || !delta)
        return;
    struct tool_track *t = track_find(s, item_id);
    if (!t)
        return;
    struct stream_event ev = {
        .kind = EV_TOOL_CALL_DELTA,
        .u.tool_call_delta = {.id = t->call_id, .args_delta = delta},
    };
    emit(s, &ev);
}

static void handle_failed(struct stream_state *s, json_t *root)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    const char *msg = "response.failed";
    json_t *resp = json_object_get(root, "response");
    if (resp) {
        json_t *err = json_object_get(resp, "error");
        if (err) {
            const char *m = json_string_value(json_object_get(err, "message"));
            if (m)
                msg = m;
        }
    }
    struct stream_event ev = {
        .kind = EV_ERROR,
        .u.error = {.message = msg, .http_status = 0},
    };
    emit(s, &ev);
}

/* response.incomplete fires when the backend truncates (hit max_output_tokens,
 * content filter, etc.). Surface as an error so the agent discards the partial
 * turn instead of committing it to history. */
static void handle_incomplete(struct stream_state *s, json_t *root)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    const char *reason = NULL;
    json_t *resp = json_object_get(root, "response");
    json_t *details = resp ? json_object_get(resp, "incomplete_details") : NULL;
    if (details)
        reason = json_string_value(json_object_get(details, "reason"));
    char *msg = xasprintf("response incomplete: %s", reason ? reason : "unknown");
    struct stream_event ev = {
        .kind = EV_ERROR,
        .u.error = {.message = msg, .http_status = 0},
    };
    emit(s, &ev);
    free(msg);
}

static void handle_completed(struct stream_state *s)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    struct stream_event ev = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = "completed"},
    };
    emit(s, &ev);
}

static int on_sse_event(const char *event_name, const char *data, void *user)
{
    (void)event_name; /* Codex mirrors the type in the data JSON */
    struct stream_state *s = user;

    if (!data || !*data)
        return 0;
    if (strcmp(data, "[DONE]") == 0) {
        handle_completed(s);
        return 0;
    }

    json_error_t err;
    json_t *root = json_loads(data, 0, &err);
    if (!root)
        return 0; /* skip unparseable */

    const char *type = json_string_value(json_object_get(root, "type"));
    if (!type) {
        json_decref(root);
        return 0;
    }

    if (strcmp(type, "response.output_item.added") == 0)
        handle_output_item_added(s, root);
    else if (strcmp(type, "response.output_item.done") == 0)
        handle_output_item_done(s, root);
    else if (strcmp(type, "response.output_text.delta") == 0)
        handle_text_delta(s, root);
    else if (strcmp(type, "response.function_call_arguments.delta") == 0)
        handle_args_delta(s, root);
    else if (strcmp(type, "response.completed") == 0 || strcmp(type, "response.done") == 0)
        handle_completed(s);
    else if (strcmp(type, "response.incomplete") == 0)
        handle_incomplete(s, root);
    else if (strcmp(type, "response.failed") == 0)
        handle_failed(s, root);
    /* all other event types are ignored for v1 */

    json_decref(root);
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

    struct stream_state state = {.user_cb = cb, .user = user};
    struct http_response resp;
    int rc = http_sse_post(CODEX_ENDPOINT, headers, body, body_len, on_sse_event, &state, &resp);

    if (resp.status == 401) {
        struct stream_event ev = {
            .kind = EV_ERROR,
            .u.error = {.message = "codex token expired — run `codex` "
                                   "once to refresh, then retry",
                        .http_status = 401},
        };
        cb(&ev, user);
    } else if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        struct stream_event ev = {
            .kind = EV_ERROR,
            .u.error = {.message = resp.error_body ? resp.error_body : "request failed",
                        .http_status = (int)resp.status},
        };
        cb(&ev, user);
    } else if (!state.terminated) {
        /* SSE connection closed cleanly without a terminal event — a proxy
         * or the backend cut the stream mid-generation. Surface as an error
         * so partial text / tool state is discarded rather than committed. */
        struct stream_event ev = {
            .kind = EV_ERROR,
            .u.error = {.message = "stream ended before completion", .http_status = 0},
        };
        cb(&ev, user);
    }

    free(resp.error_body);
    free(auth_hdr);
    free(acct_hdr);
    free(body);
    tracks_free(&state);
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
    c->base.stream = codex_stream;
    c->base.destroy = codex_destroy;
    c->access_token = xstrdup(access);
    c->account_id = xstrdup(account);

    json_decref(root);
    return &c->base;
}
