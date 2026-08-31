/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "tools/bash_cd_strip.h"
#include "tools/bash_classify.h"
#include "tools/bash_process.h"
#include "tools/task_registry.h"

/* The model cannot observe the configured ceiling, so clamp rather than requiring a retry. */
static char *resolve_timeout_ms(json_t *arguments, long *timeout_ms_out)
{
    json_t *value = json_object_get(arguments, "timeout_seconds");
    if (!value) {
        *timeout_ms_out = config_duration_ms("bash.timeout");
        return NULL;
    }
    if (!json_is_integer(value))
        return xstrdup("'timeout_seconds' must be an integer");

    long seconds = (long)json_integer_value(value);
    if (seconds < 1)
        return xstrdup("'timeout_seconds' must be >= 1");

    long timeout_ms = seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L;
    long maximum_ms = config_duration_ms("bash.timeout_max");
    if (maximum_ms > 0 && timeout_ms > maximum_ms)
        timeout_ms = maximum_ms;
    *timeout_ms_out = timeout_ms;
    return NULL;
}

static char *run_bash(const char *args_json, struct tool_run_ctx *ctx)
{
    json_error_t json_error;
    json_t *arguments = json_loads(args_json ? args_json : "{}", 0, &json_error);
    if (!arguments)
        return xasprintf("invalid arguments: %s", json_error.text);

    const char *command = json_string_value(json_object_get(arguments, "command"));
    if (!command || !*command) {
        json_decref(arguments);
        return xstrdup("missing 'command' argument");
    }

    json_t *background_value = json_object_get(arguments, "background");
    if (background_value && !json_is_boolean(background_value)) {
        json_decref(arguments);
        return xstrdup("'background' must be a boolean");
    }
    int background = background_value ? json_boolean_value(background_value) : 0;
    if (background && config_bool("no_tasks")) {
        json_decref(arguments);
        return xstrdup("background tasks are disabled; run the command synchronously");
    }

    /* Validate before the command runs, so a bad name is a side-effect-free retry. With tasks
     * disabled the name is inert and ignored. Models routinely send every declared field, so an
     * empty name means unnamed rather than forcing a retry. */
    const char *name = NULL;
    json_t *name_value = json_object_get(arguments, "name");
    if (name_value && !json_is_null(name_value) && !config_bool("no_tasks")) {
        if (!json_is_string(name_value)) {
            json_decref(arguments);
            return xstrdup("'name' must be a string");
        }
        name = json_string_value(name_value);
        if (!*name) {
            name = NULL;
        } else {
            char *name_error = task_name_error(name);
            if (name_error) {
                json_decref(arguments);
                return name_error;
            }
        }
    }

    /* Refuse before the command runs, like a bad name, so the model can kill or wait first. */
    int max_running = config_int("task.max_running");
    if (background && task_running_count() >= (size_t)max_running) {
        json_decref(arguments);
        return xasprintf("too many running tasks (max %d): wait on or kill one first", max_running);
    }

    long timeout_ms = 0;
    char *error = resolve_timeout_ms(arguments, &timeout_ms);
    if (error) {
        json_decref(arguments);
        return error;
    }

    char *result = bash_run_command(command, timeout_ms, background, name, ctx);
    json_decref(arguments);
    return result;
}

/* Return rewritten arguments only when the leading cd is proven to be a filesystem no-op. */
static char *preprocess_args(const char *args_json)
{
    if (!args_json)
        return NULL;
    json_error_t json_error;
    json_t *arguments = json_loads(args_json, 0, &json_error);
    if (!arguments)
        return NULL;
    const char *command = json_string_value(json_object_get(arguments, "command"));
    if (!command) {
        json_decref(arguments);
        return NULL;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        json_decref(arguments);
        return NULL;
    }
    size_t command_offset = bash_strip_cd_prefix(command, cwd, getenv("HOME"));
    if (command_offset == 0) {
        json_decref(arguments);
        return NULL;
    }
    json_object_set_new(arguments, "command", json_string(command + command_offset));
    char *rewritten_args = json_dumps(arguments, JSON_COMPACT);
    json_decref(arguments);
    return rewritten_args;
}

static enum tool_preview_mode select_preview(const char *args_json)
{
    if (!args_json)
        return TOOL_PREVIEW_HEAD_TAIL;
    json_error_t json_error;
    json_t *arguments = json_loads(args_json, 0, &json_error);
    if (!arguments)
        return TOOL_PREVIEW_HEAD_TAIL;
    const char *command = json_string_value(json_object_get(arguments, "command"));
    enum tool_preview_mode mode = command && bash_command_is_exploration(command)
                                      ? TOOL_PREVIEW_COLLAPSED
                                      : TOOL_PREVIEW_HEAD_TAIL;
    json_decref(arguments);
    return mode;
}

static const char BASH_DESCRIPTION[] =
    "Run a shell command via bash -c (POSIX sh -c where bash is unavailable). Returns combined "
    "stdout+stderr plus exit code.\n"
    "\n"
    "Rules:\n"
    "- Each call starts in the working directory listed under `# Environment`; `cd` does not "
    "persist across calls.\n"
    "- Follow the command preferences under `# Environment` when present.\n"
    "- Usually omit `timeout_seconds`: a command that outlives the default timeout (120s) is "
    "not killed — it detaches into a background task and you will be notified when it "
    "finishes.\n"
    "- Set `background` for commands meant to run alongside other work (servers, watchers, "
    "long builds, subagents): the call returns after a brief initial-output window and the "
    "command continues as a task. No trailing `&`: the task tracks the shell, and processes "
    "orphaned by an exited shell are killed.";

static const struct tool_param BASH_PARAMS[] = {
    {.name = "command", .type = "string", .required = 1, .description = "Shell command to run."},
    {.name = "timeout_seconds",
     .type = "integer",
     .minimum = 1,
     .description = "Optional override of the default timeout; rarely needed — on expiry the "
                    "command detaches into a background task instead of dying. The harness "
                    "clamps to a configured maximum."},
    {.name = "background",
     .type = "boolean",
     .description = "Run as a background task: return after a brief initial-output window while "
                    "the command keeps running; `timeout_seconds` is ignored. A command that "
                    "finishes within the window returns synchronously and creates no task."},
    {.name = "name",
     .type = "string",
     .description = "Optional short task name used instead of the automatic id if the command "
                    "detaches (letters/digits/-/_, max 32 chars, e.g. \"tests\")."},
};

static const char BASH_DESCRIPTION_NO_TASKS[] =
    "Run a shell command via bash -c (POSIX sh -c where bash is unavailable). Returns combined "
    "stdout+stderr plus exit code.\n"
    "\n"
    "Rules:\n"
    "- Each call starts in the working directory listed under `# Environment`; `cd` does not "
    "persist across calls.\n"
    "- Follow the command preferences under `# Environment` when present.\n"
    "- Default timeout is 120s; pass `timeout_seconds` for slow commands (test suites, builds). "
    "The harness enforces a hard ceiling.";

static const struct tool_param BASH_PARAMS_NO_TASKS[] = {
    {.name = "command", .type = "string", .required = 1, .description = "Shell command to run."},
    {.name = "timeout_seconds",
     .type = "integer",
     .minimum = 1,
     .description = "Optional override of the default timeout. Use a higher value for slow builds "
                    "or test suites; the harness clamps to a configured maximum."},
};

static const struct tool_def BASH_DEF_NO_TASKS = {
    .name = "bash",
    .description = BASH_DESCRIPTION_NO_TASKS,
    .params = BASH_PARAMS_NO_TASKS,
    .n_params = sizeof(BASH_PARAMS_NO_TASKS) / sizeof(BASH_PARAMS_NO_TASKS[0]),
};

static const struct tool_def *bash_advertise(void)
{
    return config_bool("no_tasks") ? &BASH_DEF_NO_TASKS : &TOOL_BASH.def;
}

const struct tool TOOL_BASH = {
    .def = {.name = "bash",
            .description = BASH_DESCRIPTION,
            .params = BASH_PARAMS,
            .n_params = sizeof(BASH_PARAMS) / sizeof(BASH_PARAMS[0])},
    .run = run_bash,
    .preprocess_args = preprocess_args,
    .advertise = bash_advertise,
    .display = {.arg_name = "command",
                .preview_mode = TOOL_PREVIEW_HEAD_TAIL,
                .header_rows = 3,
                .select_preview = select_preview},
};
