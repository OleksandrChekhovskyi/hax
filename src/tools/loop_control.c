/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "loop.h"
#include "tool.h"
#include "xalloc.h"

static const struct tool_param PARAMS[] = {
    {.name = "delay_seconds",
     .type = "integer",
     .description = "Delay before the next iteration, from 60 through 3600 seconds.",
     .minimum = 60},
    {.name = "stop",
     .type = "boolean",
     .description = "Stop the loop instead of scheduling another iteration."},
};

static const char *INVALID_RESULT =
    "loop_control needs delay_seconds (60..3600), or stop=true, for an active self-paced loop";

static char *run_loop_control(const char *args_json, struct tool_run_ctx *ctx)
{
    if (!ctx || !ctx->user)
        return xstrdup(INVALID_RESULT);

    json_t *root = args_json ? json_loads(args_json, 0, NULL) : NULL;
    if (!json_is_object(root)) {
        json_decref(root);
        return xstrdup(INVALID_RESULT);
    }

    json_t *stop_value = json_object_get(root, "stop");
    json_t *delay_value = json_object_get(root, "delay_seconds");
    int stop = json_is_true(stop_value);
    if (stop_value && !json_is_boolean(stop_value)) {
        json_decref(root);
        return xstrdup(INVALID_RESULT);
    }
    if (stop == (delay_value != NULL)) {
        json_decref(root);
        return xstrdup(INVALID_RESULT);
    }

    struct loop_schedule *schedule = ctx->user;
    if (stop) {
        int result = loop_schedule_control(schedule, 0, 1);
        json_decref(root);
        return xstrdup(result == 0 ? "self-paced loop stopped" : INVALID_RESULT);
    }
    if (!json_is_integer(delay_value)) {
        json_decref(root);
        return xstrdup(INVALID_RESULT);
    }

    long long seconds = json_integer_value(delay_value);
    if (seconds < LOOP_SELF_PACED_MIN_DELAY_MS / 1000 ||
        seconds > LOOP_SELF_PACED_MAX_DELAY_MS / 1000) {
        json_decref(root);
        return xstrdup(INVALID_RESULT);
    }
    int result = loop_schedule_control(schedule, (long)(seconds * 1000LL), 0);
    json_decref(root);
    if (result != 0)
        return xstrdup(INVALID_RESULT);
    return xasprintf("self-paced loop continues in %lld seconds", seconds);
}

const struct tool TOOL_LOOP_CONTROL = {
    .def =
        {
            .name = "loop_control",
            .description = "Continue or stop the active self-paced loop after this iteration.",
            .params = PARAMS,
            .n_params = sizeof(PARAMS) / sizeof(PARAMS[0]),
        },
    .run = run_loop_control,
    .display = {.preview_mode = TOOL_PREVIEW_HEAD},
};
