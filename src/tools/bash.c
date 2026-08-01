/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "tool.h"
#include "util.h"
#include "tools/bash_cd_strip.h"
#include "tools/bash_classify.h"
#include "tools/bash_process.h"

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

    long timeout_ms = 0;
    char *error = resolve_timeout_ms(arguments, &timeout_ms);
    if (error) {
        json_decref(arguments);
        return error;
    }

    char *result = bash_run_command(command, timeout_ms, ctx ? ctx->display : NULL,
                                    ctx ? ctx->display_data : NULL);
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
    "- Default timeout is 120s; pass `timeout_seconds` for slow commands (test suites, builds). "
    "The harness enforces a hard ceiling.";

static const struct tool_param BASH_PARAMS[] = {
    {.name = "command", .type = "string", .required = 1, .description = "Shell command to run."},
    {.name = "timeout_seconds",
     .type = "integer",
     .minimum = 1,
     .description = "Optional override of the default timeout. Use a higher value for slow builds "
                    "or test suites; the harness clamps to a configured maximum."},
};

const struct tool TOOL_BASH = {
    .def = {.name = "bash",
            .description = BASH_DESCRIPTION,
            .params = BASH_PARAMS,
            .n_params = sizeof(BASH_PARAMS) / sizeof(BASH_PARAMS[0])},
    .run = run_bash,
    .preprocess_args = preprocess_args,
    .display = {.arg_name = "command",
                .preview_mode = TOOL_PREVIEW_HEAD_TAIL,
                .header_rows = 3,
                .select_preview = select_preview},
};
