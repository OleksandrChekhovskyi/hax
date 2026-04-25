/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_H
#define HAX_TOOL_H

#include "provider.h"

/*
 * A tool bundles its metadata (passed to the model) with the function that
 * runs it. Each tool lives in its own translation unit under src/tools/ and
 * exports exactly one `const struct tool` symbol.
 *
 * run() takes a JSON-encoded argument string and returns a freshly allocated
 * output string that will be shown back to the model. On error, the returned
 * string explains the failure — the model treats it as tool output and can
 * recover. Returning NULL is not allowed.
 *
 * output_is_diff hints that successful output is a unified diff and the
 * agent should render it colored (and uncapped) instead of as a dim
 * preview. Failure messages from the same tool are not diffs; the agent
 * routes by checking whether the output starts with `--- `.
 */
struct tool {
    struct tool_def def;
    char *(*run)(const char *args_json);
    /* Optional. Returns a small dim suffix appended to the tool-call header
     * after the bold `display_arg` value — e.g. `:5-20` for `read` to
     * surface the requested line range. NULL or empty string means no
     * suffix. Caller frees the returned string. */
    char *(*format_display_extra)(const char *args_json);
    int output_is_diff;
};

extern const struct tool TOOL_READ;
extern const struct tool TOOL_BASH;
extern const struct tool TOOL_WRITE;
extern const struct tool TOOL_EDIT;

#endif /* HAX_TOOL_H */
