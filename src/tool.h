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
 */
struct tool {
    struct tool_def def;
    char *(*run)(const char *args_json);
};

extern const struct tool TOOL_READ;
extern const struct tool TOOL_BASH;

#endif /* HAX_TOOL_H */
