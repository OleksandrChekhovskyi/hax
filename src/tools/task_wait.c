/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <limits.h>
#include <stdio.h>

#include "buf.h"
#include "config.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "text/fmt.h"
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

    json_t *kill_value = json_object_get(arguments, "kill");
    if (kill_value && !json_is_boolean(kill_value)) {
        json_decref(arguments);
        return xstrdup("'kill' must be a boolean");
    }
    int kill_on_timeout = kill_value ? json_boolean_value(kill_value) : 0;

    /* A kill with no timeout is immediate; a plain wait falls back to the configured window. */
    long timeout_ms = kill_on_timeout ? 0 : config_duration_ms("task.wait_timeout");
    json_t *timeout_value = json_object_get(arguments, "timeout_seconds");
    if (timeout_value) {
        if (!json_is_integer(timeout_value) || json_integer_value(timeout_value) < 0) {
            json_decref(arguments);
            return xstrdup("'timeout_seconds' must be an integer >= 0");
        }
        long seconds = (long)json_integer_value(timeout_value);
        timeout_ms = seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L;
    }

    char *report = task_wait_stream(id, timeout_ms, kill_on_timeout, ctx ? ctx->display : NULL,
                                    ctx ? ctx->display_data : NULL);
    json_decref(arguments);
    return report;
}

static const char TASK_WAIT_DESCRIPTION[] =
    "Wait on one background task; returns the output it produced since you last saw it plus "
    "its status.\n"
    "\n"
    "Returns immediately for an already-finished task (this is also how you collect a task "
    "announced as finished), and returns early when a different task finishes so you can react "
    "to it. Wait on the task whose result you need next; do not poll in a loop of short waits.\n"
    "\n"
    "With `kill`, the task is stopped (SIGTERM, then SIGKILL after a grace period) and its "
    "final output is returned; add `timeout_seconds` to first give the task that long to "
    "finish on its own.";

static const struct tool_param TASK_WAIT_PARAMS[] = {
    {.name = "id",
     .type = "string",
     .required = 1,
     .description = "Task id to wait on (e.g. \"t1\")."},
    {.name = "timeout_seconds",
     .type = "integer",
     .description = "How long to block waiting for the task to finish; 0 does not block. "
                    "Defaults to a configured value (10 minutes unless changed); with `kill` "
                    "it defaults to 0 (kill immediately)."},
    {.name = "kill",
     .type = "boolean",
     .description = "Kill the task and report its final output. With `timeout_seconds`, the "
                    "task first gets that window to finish on its own; the kill fires only if "
                    "it is still running when the timeout elapses."},
};

static const struct tool_def *task_wait_advertise(void)
{
    return config_bool("no_tasks") ? NULL : &TOOL_TASK_WAIT.def;
}

/* "t1", "t1 (up to 30s)", "t1 (kill)", "t1 (up to 30s, then kill)" — malformed arguments fall
 * back to raw JSON. */
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
    int kill = json_boolean_value(json_object_get(arguments, "kill"));
    json_t *timeout = json_object_get(arguments, "timeout_seconds");
    if (json_is_integer(timeout) && json_integer_value(timeout) > 0) {
        long seconds = (long)json_integer_value(timeout);
        char duration[32];
        format_duration(duration, sizeof(duration),
                        seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L);
        char suffix[64];
        if (kill)
            snprintf(suffix, sizeof(suffix), " (up to %s, then kill)", duration);
        else
            snprintf(suffix, sizeof(suffix), " (up to %s)", duration);
        buf_append_str(&out, suffix);
    } else if (kill) {
        buf_append_str(&out, " (kill)");
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
