/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>

#include "config.h"
#include "tool.h"
#include "util.h"
#include "tools/task_registry.h"

static char *run_task_kill(const char *args_json, struct tool_run_ctx *ctx)
{
    (void)ctx;
    if (config_bool("no_tasks"))
        return xstrdup("background tasks are disabled");

    json_error_t json_error;
    json_t *arguments = json_loads(args_json ? args_json : "{}", 0, &json_error);
    if (!arguments)
        return xasprintf("invalid arguments: %s", json_error.text);

    json_t *ids_value = json_object_get(arguments, "ids");
    if (!json_is_array(ids_value)) {
        json_decref(arguments);
        return xstrdup("'ids' must be an array of task ids, e.g. [\"t1\"]");
    }
    size_t n_ids = json_array_size(ids_value);
    const char **ids = xmalloc(n_ids * sizeof(*ids));
    for (size_t i = 0; i < n_ids; i++) {
        ids[i] = json_string_value(json_array_get(ids_value, i));
        if (!ids[i]) {
            free(ids);
            json_decref(arguments);
            return xstrdup("'ids' must contain only strings");
        }
    }

    char *report = task_kill_report(ids, n_ids);
    free(ids);
    json_decref(arguments);
    return report;
}

static const char TASK_KILL_DESCRIPTION[] =
    "Stop background tasks: SIGTERM, then SIGKILL after a grace period. Returns one status "
    "line per task; any output you have not seen remains collectable with task_wait.";

static const struct tool_param TASK_KILL_PARAMS[] = {
    {.name = "ids",
     .type = "array",
     .item_type = "string",
     .required = 1,
     .description = "Task ids to stop (e.g. [\"t1\"])."},
};

static const struct tool_def *task_kill_advertise(void)
{
    return config_bool("no_tasks") ? NULL : &TOOL_TASK_KILL.def;
}

/* "t1, t2" — malformed arguments fall back to raw JSON. */
static char *format_kill_argument(const char *args_json)
{
    json_t *arguments = json_loads(args_json, 0, NULL);
    if (!arguments)
        return NULL;
    json_t *ids = json_object_get(arguments, "ids");
    size_t n_ids = json_is_array(ids) ? json_array_size(ids) : 0;
    struct buf out;
    buf_init(&out);
    for (size_t i = 0; i < n_ids; i++) {
        const char *id = json_string_value(json_array_get(ids, i));
        if (!id) {
            buf_free(&out);
            json_decref(arguments);
            return NULL;
        }
        if (i > 0)
            buf_append_str(&out, ", ");
        buf_append_str(&out, id);
    }
    json_decref(arguments);
    if (out.len == 0) {
        buf_free(&out);
        return NULL;
    }
    return buf_steal(&out);
}

const struct tool TOOL_TASK_KILL = {
    .def = {.name = "task_kill",
            .description = TASK_KILL_DESCRIPTION,
            .params = TASK_KILL_PARAMS,
            .n_params = sizeof(TASK_KILL_PARAMS) / sizeof(TASK_KILL_PARAMS[0])},
    .run = run_task_kill,
    .advertise = task_kill_advertise,
    .display = {.format_argument = format_kill_argument, .preview_mode = TOOL_PREVIEW_HEAD_TAIL},
};
