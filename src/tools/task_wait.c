/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <limits.h>
#include <stdio.h>

#include "config.h"
#include "provider.h"
#include "tool.h"
#include "util.h"
#include "tools/task_registry.h"

static char *run_task_wait(const char *args_json, struct tool_run_ctx *ctx)
{
    if (config_bool("no_tasks"))
        return xstrdup("background tasks are disabled");

    json_error_t json_error;
    json_t *arguments = json_loads(args_json ? args_json : "{}", 0, &json_error);
    if (!arguments)
        return xasprintf("invalid arguments: %s", json_error.text);

    json_t *id_value = json_object_get(arguments, "id");
    const char *id = json_string_value(id_value);
    if (!id || !*id) {
        json_decref(arguments);
        return xstrdup("missing 'id': name the task to wait on, e.g. \"t1\"");
    }

    long timeout_ms = config_duration_ms("task.wait_timeout");
    json_t *timeout_value = json_object_get(arguments, "timeout_seconds");
    if (timeout_value) {
        if (!json_is_integer(timeout_value) || json_integer_value(timeout_value) < 1) {
            json_decref(arguments);
            return xstrdup("'timeout_seconds' must be an integer >= 1");
        }
        long seconds = (long)json_integer_value(timeout_value);
        timeout_ms = seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L;
    }

    char *report =
        task_wait_stream(id, timeout_ms, ctx ? ctx->display : NULL, ctx ? ctx->display_data : NULL);
    json_decref(arguments);
    return report;
}

static const char TASK_WAIT_DESCRIPTION[] =
    "Wait on one background task, streaming its output; returns the output produced since you "
    "last saw it plus the task's status.\n"
    "\n"
    "Returns immediately for an already-finished task (this is also how you collect a task "
    "announced as finished), and returns early when a different task finishes so you can react "
    "to it. Wait on the task whose result you need next; do not poll in a loop of short waits.";

static const struct tool_param TASK_WAIT_PARAMS[] = {
    {.name = "id",
     .type = "string",
     .required = 1,
     .description = "Task id to wait on (e.g. \"t1\")."},
    {.name = "timeout_seconds",
     .type = "integer",
     .minimum = 1,
     .description = "How long to block before reporting the task still running. Defaults to a "
                    "configured value (10 minutes unless changed)."},
};

static const struct tool_def *task_wait_advertise(void)
{
    return config_bool("no_tasks") ? NULL : &TOOL_TASK_WAIT.def;
}

/* "t1", optionally "(up to 30s)" — malformed arguments fall back to raw JSON. */
static char *format_wait_argument(const char *args_json)
{
    json_t *arguments = json_loads(args_json, 0, NULL);
    if (!arguments)
        return NULL;
    const char *id = json_string_value(json_object_get(arguments, "id"));
    if (!id || !*id) {
        json_decref(arguments);
        return NULL;
    }
    struct buf out;
    buf_init(&out);
    buf_append_str(&out, id);
    json_t *timeout = json_object_get(arguments, "timeout_seconds");
    if (json_is_integer(timeout) && json_integer_value(timeout) > 0) {
        long seconds = (long)json_integer_value(timeout);
        char duration[32];
        format_duration(duration, sizeof(duration),
                        seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L);
        char suffix[48];
        snprintf(suffix, sizeof(suffix), " (up to %s)", duration);
        buf_append_str(&out, suffix);
    }
    json_decref(arguments);
    return buf_steal(&out);
}

const struct tool TOOL_TASK_WAIT = {
    .def = {.name = "task_wait",
            .description = TASK_WAIT_DESCRIPTION,
            .params = TASK_WAIT_PARAMS,
            .n_params = sizeof(TASK_WAIT_PARAMS) / sizeof(TASK_WAIT_PARAMS[0])},
    .run = run_task_wait,
    .advertise = task_wait_advertise,
    .display = {.format_argument = format_wait_argument, .preview_mode = TOOL_PREVIEW_HEAD_TAIL},
};
