/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_H
#define HAX_TOOL_H

#include "provider.h"

/* Synchronous display-only sink; bytes are borrowed for the callback. */
typedef void (*tool_display_fn)(const char *bytes, size_t n, void *data);

/* Per-call channels for tool execution. NULL and zero-initialized contexts disable display and
 * image attachment. Ownership of result_images transfers to the caller after run returns. */
struct tool_run_ctx {
    tool_display_fn display;
    void *display_data;
    /* Same tri-state convention as context.image_input; zero means unsupported. */
    int image_input;
    struct item_image *result_images; /* owned array of owned members */
    size_t n_result_images;
    /* The returned output summarizes bytes sent through display instead of repeating them. */
    int output_summarizes_display;
    /* The run was cut short by a user interrupt; the result carries interrupted provenance. */
    int interrupted;
    /* Length of a trailing model-only annotation in the returned output; user-facing rendering
     * omits it. Control-free text only, so stripping the stored copy keeps the length valid. */
    size_t output_hidden_tail;
};

enum tool_output_style {
    TOOL_OUTPUT_PLAIN,
    TOOL_OUTPUT_UNIFIED_DIFF,
};

enum tool_preview_mode {
    TOOL_PREVIEW_HEAD,
    TOOL_PREVIEW_HEAD_TAIL,
    TOOL_PREVIEW_COLLAPSED,
};

struct tool_display {
    /* Name of the JSON argument shown after the tool name. */
    const char *arg_name;
    /* Optional allocated replacement for the displayed argument, for tools whose arguments
     * are not a single string; the caller frees it. NULL results fall back to arg_name
     * extraction and then to the raw JSON. */
    char *(*format_argument)(const char *args_json);
    enum tool_output_style output_style;
    enum tool_preview_mode preview_mode;
    /* Zero uses the one-row default. */
    int header_rows;
    /* Optional allocated suffix for the displayed argument; the caller frees it. */
    char *(*format_extra)(const char *args_json);
    /* Optional per-call override of preview_mode. */
    enum tool_preview_mode (*select_preview)(const char *args_json);
    /* Optional allocated rewrite of the displayed argument for collapsed rows; `argument` is
     * the resolved display argument (NULL when the call had none). The caller frees it. */
    char *(*collapse_argument)(const char *argument);
};

struct tool {
    struct tool_def def;
    /* args_json may be NULL. Returns allocated model-facing output, including recoverable errors;
     * returning NULL is invalid. */
    char *(*run)(const char *args_json, struct tool_run_ctx *ctx);
    /* Returns allocated arguments for local execution, or NULL to use the original payload. */
    char *(*preprocess_args)(const char *args_json);
    /* Optional configuration-dependent advertisement: the def to offer the model, or NULL to
     * withhold the tool entirely. NULL hook means always advertise `def`. Dispatch is not
     * gated by this — a withheld tool's run() must answer stray calls with a recoverable
     * error. */
    const struct tool_def *(*advertise)(void);
    struct tool_display display;
};

extern const struct tool TOOL_READ;
extern const struct tool TOOL_EDIT;
extern const struct tool TOOL_WRITE;
extern const struct tool TOOL_BASH;
extern const struct tool TOOL_TASK_WAIT;

#endif /* HAX_TOOL_H */
