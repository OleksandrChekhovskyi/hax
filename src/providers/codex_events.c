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

    if (strcmp(type, "function_call") == 0) {
        const char *item_id = json_string_value(json_object_get(item, "id"));
        struct codex_tool_track *t = track_find(s, item_id);
        if (!t)
            return;
        struct stream_event ev = {
            .kind = EV_TOOL_CALL_END,
            .u.tool_call_end = {.id = t->call_id},
        };
        emit(s, &ev);
        return;
    }

    if (strcmp(type, "reasoning") == 0) {
        /* Skip if encrypted_content is missing or null — without it the
         * item is useless for round-trip (we never see the plaintext),
         * and sending bare summary back would be noise. Shouldn't happen
         * when we ask for it via `include`, but the server may emit
         * empty reasoning items in some edge cases. */
        json_t *enc = json_object_get(item, "encrypted_content");
        if (!enc || json_is_null(enc))
            return;

        /* Whitelist the fields valid as Responses API *input* items.
         * Matches codex-rs's serde annotations on ResponseItem::Reasoning:
         * `id` is skip_serializing (it identifies the *output* item, not
         * an input one), `content` is skipped when it doesn't carry
         * reasoning_text (always our case — we ask for encrypted_content
         * instead). Whitelisting also future-proofs us against new output
         * fields the server may add: anything we don't know about stays
         * out of the next request. */
        json_t *clean = json_object();
        json_object_set_new(clean, "type", json_string("reasoning"));
        json_t *summary = json_object_get(item, "summary");
        if (summary)
            json_object_set(clean, "summary", summary);
        else
            json_object_set_new(clean, "summary", json_array());
        json_object_set(clean, "encrypted_content", enc);

        char *json = json_dumps(clean, JSON_COMPACT);
        json_decref(clean);
        if (!json)
            return;
        struct stream_event ev = {
            .kind = EV_REASONING_ITEM,
            .u.reasoning_item = {.json = json},
        };
        emit(s, &ev);
        free(json);
    }
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

/* Codex carries usage on the final response.completed event under
 * response.usage. cached_tokens lives in input_tokens_details — when absent
 * we leave it at -1 ("unknown"), which is distinct from 0 ("known to be no
 * cache hit"). */
static void parse_usage(json_t *root, struct stream_usage *out)
{
    out->input_tokens = -1;
    out->output_tokens = -1;
    out->cached_tokens = -1;

    json_t *resp = json_object_get(root, "response");
    json_t *usage = resp ? json_object_get(resp, "usage") : NULL;
    if (!json_is_object(usage))
        return;

    json_t *v = json_object_get(usage, "input_tokens");
    if (json_is_integer(v))
        out->input_tokens = (long)json_integer_value(v);
    v = json_object_get(usage, "output_tokens");
    if (json_is_integer(v))
        out->output_tokens = (long)json_integer_value(v);

    json_t *details = json_object_get(usage, "input_tokens_details");
    if (json_is_object(details)) {
        v = json_object_get(details, "cached_tokens");
        if (json_is_integer(v))
            out->cached_tokens = (long)json_integer_value(v);
    }
}

static void handle_completed(struct codex_events *s, json_t *root)
{
    if (s->terminated)
        return;
    s->terminated = 1;
    struct stream_event ev = {
        .kind = EV_DONE,
        .u.done = {.stop_reason = "completed"},
    };
    if (root)
        parse_usage(root, &ev.u.done.usage);
    else
        ev.u.done.usage = (struct stream_usage){-1, -1, -1};
    emit(s, &ev);
}

void codex_events_feed(struct codex_events *s, const char *data)
{
    if (!data || !*data)
        return;
    if (strcmp(data, "[DONE]") == 0) {
        handle_completed(s, NULL);
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
    else if (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
             strcmp(type, "response.reasoning_text.delta") == 0) {
        /* Visible CoT or summary deltas — we don't display or store the
         * text (encrypted_content on the final reasoning item is what
         * round-trips), but the activity drives the "thinking..." spinner.
         * Symmetric with openai_events: skip empty/missing deltas so a
         * malformed event doesn't fire a UX signal with no content. */
        const char *delta = json_string_value(json_object_get(root, "delta"));
        if (delta && *delta) {
            struct stream_event ev = {
                .kind = EV_REASONING_DELTA,
                .u.reasoning_delta = {.text = delta},
            };
            emit(s, &ev);
        }
    } else if (strcmp(type, "response.function_call_arguments.delta") == 0)
        handle_args_delta(s, root);
    else if (strcmp(type, "response.completed") == 0 || strcmp(type, "response.done") == 0)
        handle_completed(s, root);
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
