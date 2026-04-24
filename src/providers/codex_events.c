/* SPDX-License-Identifier: MIT */
#include "codex_events.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void codex_events_init(struct codex_events *s, stream_cb cb, void *user)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->user = user;
}

void codex_events_free(struct codex_events *s)
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

static struct codex_tool_track *track_find(struct codex_events *s, const char *item_id)
{
    if (!item_id)
        return NULL;
    for (size_t i = 0; i < s->n_tools; i++) {
        if (s->tools[i].item_id && strcmp(s->tools[i].item_id, item_id) == 0)
            return &s->tools[i];
    }
    return NULL;
}

static struct codex_tool_track *track_add(struct codex_events *s, const char *item_id,
                                          const char *call_id, const char *name)
{
    if (s->n_tools == s->cap_tools) {
        size_t cap = s->cap_tools ? s->cap_tools * 2 : 4;
        s->tools = xrealloc(s->tools, cap * sizeof(*s->tools));
        s->cap_tools = cap;
    }
    struct codex_tool_track *t = &s->tools[s->n_tools++];
    t->item_id = xstrdup(item_id);
    t->call_id = xstrdup(call_id);
    t->name = xstrdup(name);
    return t;
}

static int emit(struct codex_events *s, const struct stream_event *ev)
{
    return s->cb(ev, s->user);
}

static void handle_output_item_added(struct codex_events *s, json_t *root)
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

static void handle_output_item_done(struct codex_events *s, json_t *root)
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
    struct codex_tool_track *t = track_find(s, item_id);
    if (!t)
        return;

    struct stream_event ev = {
        .kind = EV_TOOL_CALL_END,
        .u.tool_call_end = {.id = t->call_id},
    };
    emit(s, &ev);
}

static void handle_text_delta(struct codex_events *s, json_t *root)
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

static void handle_args_delta(struct codex_events *s, json_t *root)
{
    const char *item_id = json_string_value(json_object_get(root, "item_id"));
    const char *delta = json_string_value(json_object_get(root, "delta"));
    if (!item_id || !delta)
        return;
    struct codex_tool_track *t = track_find(s, item_id);
    if (!t)
        return;
    struct stream_event ev = {
        .kind = EV_TOOL_CALL_DELTA,
        .u.tool_call_delta = {.id = t->call_id, .args_delta = delta},
    };
    emit(s, &ev);
}

static void handle_failed(struct codex_events *s, json_t *root)
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
static void handle_incomplete(struct codex_events *s, json_t *root)
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

static void handle_completed(struct codex_events *s)
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

void codex_events_feed(struct codex_events *s, const char *data)
{
    if (!data || !*data)
        return;
    if (strcmp(data, "[DONE]") == 0) {
        handle_completed(s);
        return;
    }

    json_error_t err;
    json_t *root = json_loads(data, 0, &err);
    if (!root)
        return; /* skip unparseable */

    const char *type = json_string_value(json_object_get(root, "type"));
    if (!type) {
        json_decref(root);
        return;
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
}

void codex_events_finalize(struct codex_events *s)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    struct stream_event ev = {
        .kind = EV_ERROR,
        .u.error = {.message = "stream ended before completion", .http_status = 0},
    };
    emit(s, &ev);
}
