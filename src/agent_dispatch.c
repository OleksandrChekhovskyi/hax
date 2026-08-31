/* SPDX-License-Identifier: MIT */
#include "agent_dispatch.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "agent_tool.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/render_ctx.h"
#include "render/spinner.h"
#include "render/tool_render.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "terminal/width.h"
#include "text/width.h"

#define HEADER_EXTRA_MAX_CELLS 20
#define MIN_DISPLAY_CELLS      8

static const char *tool_name(const struct item *call)
{
    return call->tool_name ? call->tool_name : "?";
}

static int tool_tag_cells(const char *name)
{
    return (int)display_cells(name) + 3; /* "[name] " */
}

enum tool_preview_mode tool_call_preview_mode(const struct item *call)
{
    const struct tool *tool = call && call->tool_name ? agent_find_tool(call->tool_name) : NULL;
    if (!tool)
        return TOOL_PREVIEW_HEAD;
    if (tool->display.select_preview)
        return tool->display.select_preview(call->tool_arguments_json);
    return tool->display.preview_mode;
}

static int tool_has_diff_output(const struct tool *tool)
{
    return tool && tool->display.output_style == TOOL_OUTPUT_UNIFIED_DIFF;
}

static char *display_argument(const struct tool *tool, const char *args_json)
{
    if (!tool || !args_json)
        return NULL;
    if (tool->display.format_argument) {
        char *formatted = tool->display.format_argument(args_json);
        if (formatted)
            return formatted;
    }
    if (!tool->display.arg_name)
        return NULL;
    json_t *root = json_loads(args_json, 0, NULL);
    if (!root)
        return NULL;
    const char *value = json_string_value(json_object_get(root, tool->display.arg_name));
    char *argument = value ? xstrdup(value) : NULL;
    json_decref(root);
    return argument;
}

static char *display_extra(const struct tool *tool, const char *args_json, int terminal_width)
{
    if (!tool || !tool->display.format_extra)
        return NULL;
    char *extra = tool->display.format_extra(args_json);
    if (!extra || !*extra)
        return extra;

    int max_cells = HEADER_EXTRA_MAX_CELLS;
    if (max_cells > terminal_width - MIN_DISPLAY_CELLS)
        max_cells = terminal_width - MIN_DISPLAY_CELLS;
    if (max_cells < 4)
        max_cells = 4;
    if ((int)display_cells(extra) <= max_cells)
        return extra;

    char *trimmed = truncate_for_display(extra, (size_t)max_cells);
    free(extra);
    return trimmed;
}

static void write_tool_header(struct disp *disp, const struct item *call)
{
    const char *name = tool_name(call);
    const struct tool *tool = agent_find_tool(name);
    char *argument = display_argument(tool, call->tool_arguments_json);
    int terminal_width = display_width();
    int tag_cells = tool_tag_cells(name);

    disp_block_separator(disp);
    disp_write_ansi(disp, theme_open(THEME_CHROME));
    disp_printf(disp, "[%s]", name);
    disp_write_ansi(disp, ANSI_RESET);

    if (argument) {
        char *extra = display_extra(tool, call->tool_arguments_json, terminal_width);
        int extra_cells = extra ? (int)display_cells(extra) : 0;
        int rows = tool->display.header_rows > 0 ? tool->display.header_rows : 1;
        int first_row_width = terminal_width - tag_cells;
        if (first_row_width < MIN_DISPLAY_CELLS)
            first_row_width = MIN_DISPLAY_CELLS;
        int continuation_width = terminal_width;
        if (continuation_width < MIN_DISPLAY_CELLS)
            continuation_width = MIN_DISPLAY_CELLS;

        char *flattened = flatten_for_display(argument);
        char *layout =
            reflow_for_display(flattened, first_row_width, continuation_width, rows, extra_cells);
        free(flattened);

        disp_putc(disp, ' ');
        disp_write_ansi(disp, ANSI_BOLD);
        disp_write(disp, layout, strlen(layout));
        disp_write_ansi(disp, ANSI_RESET);
        free(layout);
        if (extra && *extra) {
            disp_write_ansi(disp, ANSI_DIM);
            disp_write(disp, extra, strlen(extra));
            disp_write_ansi(disp, ANSI_RESET);
        }
        free(extra);
    } else if (call->tool_arguments_json && *call->tool_arguments_json) {
        int available_cells = terminal_width - tag_cells;
        if (available_cells < MIN_DISPLAY_CELLS)
            available_cells = MIN_DISPLAY_CELLS;
        char *flattened = flatten_for_display(call->tool_arguments_json);
        char *trimmed = truncate_for_display(flattened, (size_t)available_cells);
        free(flattened);

        disp_putc(disp, ' ');
        disp_write_ansi(disp, ANSI_DIM);
        disp_write(disp, trimmed, strlen(trimmed));
        disp_write_ansi(disp, ANSI_RESET);
        free(trimmed);
    }
    free(argument);

    disp_putc(disp, '\n');
    /* Commit the newline before the spinner or output can overprint it. */
    disp_commit_newlines(disp);
    disp_flush(disp);
}

/* Read headers remain open so later reads can coalesce on the same row. */
static int write_collapsed_header(struct disp *disp, const struct item *call, const char *argument)
{
    const char *name = tool_name(call);
    int cells = tool_tag_cells(name);

    disp_write_ansi(disp, theme_open(THEME_CHROME_DIM));
    disp_printf(disp, "[%s]", name);
    /* The theme closer may clear intensity, so restore dim for the argument. */
    disp_write_ansi(disp, theme_close(THEME_CHROME_DIM));
    disp_write_ansi(disp, ANSI_DIM);
    disp_putc(disp, ' ');
    if (argument && *argument) {
        disp_write(disp, argument, strlen(argument));
        cells += (int)display_cells(argument);
    }
    disp_write_ansi(disp, ANSI_RESET);
    disp_commit_newlines(disp);
    disp_flush(disp);
    return cells;
}

static int append_collapsed_argument(struct disp *disp, const char *argument)
{
    disp_write_ansi(disp, ANSI_DIM);
    disp_write(disp, ", ", 2);
    disp_write(disp, argument, strlen(argument));
    disp_write_ansi(disp, ANSI_RESET);
    disp_commit_newlines(disp);
    disp_flush(disp);
    return 2 + (int)display_cells(argument);
}

/* Collapsed rows are single-line; tools may rewrite the argument so coalesced calls scan
 * as a list. */
static char *collapsed_argument(const struct tool *tool, const struct item *call)
{
    if (!tool || !tool->display.arg_name || !call->tool_arguments_json)
        return xstrdup("");

    char *argument = display_argument(tool, call->tool_arguments_json);
    if (tool->display.collapse_argument) {
        char *collapsed = tool->display.collapse_argument(argument);
        free(argument);
        argument = collapsed;
    }
    if (!argument)
        return xstrdup("");

    char *extra =
        tool->display.format_extra ? tool->display.format_extra(call->tool_arguments_json) : NULL;
    char *with_extra = extra && *extra ? xasprintf("%s%s", argument, extra) : xstrdup(argument);
    free(extra);
    free(argument);
    char *flattened = flatten_for_display(with_extra);
    free(with_extra);
    return flattened;
}

/* Keep one cell free to avoid deferred terminal autowrap. */
static char *truncate_collapsed_argument(const struct tool *tool, const struct item *call,
                                         int tag_cells, int terminal_width)
{
    int available_cells = terminal_width - tag_cells - 1;
    if (available_cells < MIN_DISPLAY_CELLS)
        available_cells = MIN_DISPLAY_CELLS;
    char *argument = collapsed_argument(tool, call);
    char *trimmed = truncate_for_display(argument, (size_t)available_cells);
    free(argument);
    return trimmed;
}

void render_tool_call_header(struct render_ctx *render, const struct item *call)
{
    write_tool_header(&render->disp, call);
}

/* Use the same local argument rewrite whether or not the call runs. */
static void write_undispatched_header(struct disp *disp, const struct item *call)
{
    struct agent_tool_call prepared;
    agent_tool_call_init(&prepared, call);
    write_tool_header(disp, &prepared.effective);
    agent_tool_call_destroy(&prepared);
}

static struct item dispatch_without_run(struct render_ctx *render, const struct item *call,
                                        const char *marker, const char *output,
                                        enum item_origin origin)
{
    struct disp *disp = &render->disp;
    write_undispatched_header(disp, call);
    tool_render_write_marker_gutter(disp);
    disp_write_ansi(disp, ANSI_DIM);
    disp_write(disp, marker, strlen(marker));
    disp_write_ansi(disp, ANSI_RESET);
    disp_putc(disp, '\n');
    disp_flush(disp);

    struct item result = agent_tool_result_make(call, output, NULL);
    result.origin = origin;
    return result;
}

struct item dispatch_tool_skipped(struct render_ctx *render, const struct item *call)
{
    return dispatch_without_run(render, call, INTERRUPT_MARKER, INTERRUPT_MARKER,
                                ITEM_ORIGIN_SKIPPED);
}

struct item dispatch_tool_refused(struct render_ctx *render, const struct item *call)
{
    return dispatch_without_run(render, call, REFUSED_MARKER, REFUSED_RESULT, ITEM_ORIGIN_REFUSED);
}

static void close_collapsed_line(struct disp *disp)
{
    disp_putc(disp, '\n');
    disp_commit_newlines(disp);
}

static void write_cluster_line(struct render_ctx *render, const struct item *call)
{
    struct disp *disp = &render->disp;
    const struct tool *tool = call->tool_name ? agent_find_tool(call->tool_name) : NULL;
    int terminal_width = display_width();
    int is_read = call->tool_name && strcmp(call->tool_name, "read") == 0;
    int can_coalesce = render->cluster.line_open && render->cluster.last_tool &&
                       strcmp(render->cluster.last_tool, "read") == 0 && is_read;

    if (can_coalesce) {
        char *argument = collapsed_argument(tool, call);
        int appended_cells = 2 + (int)display_cells(argument);
        if (render->cluster.line_cells + appended_cells > terminal_width - 1) {
            close_collapsed_line(disp);
            char *trimmed = truncate_collapsed_argument(tool, call, tool_tag_cells(tool_name(call)),
                                                        terminal_width);
            render->cluster.line_cells = write_collapsed_header(disp, call, trimmed);
            free(trimmed);
        } else {
            render->cluster.line_cells += append_collapsed_argument(disp, argument);
        }
        free(argument);
    } else {
        if (render->cluster.line_open)
            close_collapsed_line(disp);
        char *trimmed = truncate_collapsed_argument(tool, call, tool_tag_cells(tool_name(call)),
                                                    terminal_width);
        render->cluster.line_cells = write_collapsed_header(disp, call, trimmed);
        free(trimmed);
        if (!is_read)
            close_collapsed_line(disp);
    }
    render->cluster.line_open = is_read;
    render->cluster.last_tool = tool ? tool->def.name : NULL;
}

void render_collapsed_tool_call(struct render_ctx *render, const struct item *call)
{
    write_cluster_line(render, call);
}

static struct item dispatch_tool_call_collapsed(struct render_ctx *render,
                                                struct agent_tool_call *prepared, int image_input)
{
    const struct item *call = &prepared->effective;
    struct spinner *spinner = render->spinner;
    int is_read = strcmp(call->tool_name, "read") == 0;

    /* The swap keeps the indicator continuous: the erase that restores the cursor to an open
     * read line, the appended cluster text, and the reparked spinner land as one frame. */
    spinner_swap_begin(spinner);
    write_cluster_line(render, call);

    /* Preserve the open line's cursor column for the next read. */
    char label[64];
    char request_key[32];
    snprintf(label, sizeof(label), "[%s] running...", call->tool_name);
    snprintf(request_key, sizeof(request_key), "run:%s", call->tool_name);
    spinner_request_label(spinner, request_key, label);
    spinner_park(spinner, is_read ? render->cluster.line_cells : 0);
    spinner_swap_end(spinner);

    struct tool_run_ctx run_ctx = {.image_input = image_input};
    char *output = agent_tool_call_run(prepared, &run_ctx);
    spinner_request_label(spinner, "working", "working...");

    struct item result = agent_tool_result_make(call, output, &run_ctx);
    free(output);
    return result;
}

void render_tool_solo_marker(struct render_ctx *render, const char *text)
{
    struct disp *disp = &render->disp;
    /* Replacing the spinner (or a live placeholder row) with the marker is a swap, so it
     * cannot render as blank-and-repaint. */
    spinner_swap_begin(render->spinner);
    tool_render_write_marker_gutter(disp);
    disp_write_ansi(disp, ANSI_DIM);
    int available_cells = display_width() - TOOL_RENDER_GUTTER_COLS;
    if (available_cells < MIN_DISPLAY_CELLS)
        available_cells = MIN_DISPLAY_CELLS;
    char *flattened = flatten_for_display(text);
    char *line = truncate_for_display(flattened, (size_t)available_cells);
    free(flattened);
    disp_write(disp, line, strlen(line));
    free(line);
    disp_write_ansi(disp, ANSI_RESET);
    disp_putc(disp, '\n');
    spinner_swap_end(render->spinner);
    disp_flush(disp);
}

/* Display callbacks are user-facing only; agent_tool_result_make normalizes stored output. */
static struct item dispatch_tool_call_verbose(struct render_ctx *render,
                                              struct agent_tool_call *prepared,
                                              enum tool_preview_mode preview_mode, int image_input)
{
    const struct item *call = &prepared->effective;
    const struct tool *tool = prepared->tool;
    struct disp *disp = &render->disp;
    struct spinner *spinner = render->spinner;
    write_tool_header(disp, call);
    /* A deferred "running" label could surface after the tool-status row releases the spinner. */
    spinner_set_label(spinner, "working", "working...");

    /* Diff mode is selected from the completed output so errors remain plain previews. */
    enum tool_render_mode mode =
        preview_mode == TOOL_PREVIEW_HEAD_TAIL ? TOOL_RENDER_HEAD_TAIL : TOOL_RENDER_HEAD;
    struct tool_render renderer;
    tool_render_init(&renderer, disp, spinner, mode);
    /* An empty live row instead of a parked "working..." label: the indicator sits in the block
     * and the first output line replaces it in place, so a fast tool never blinks a label. */
    tool_render_begin_live(&renderer);
    struct tool_run_ctx run_ctx = {
        .display = tool_render_emit,
        .display_data = &renderer,
        .image_input = image_input,
    };
    char *output = agent_tool_call_run(prepared, &run_ctx);

    if (output && !renderer.display_was_called) {
        if (tool_has_diff_output(tool) && !*output) {
            render_tool_solo_marker(render, "(no changes)");
        } else {
            if (tool_has_diff_output(tool) && strncmp(output, "--- ", 4) == 0)
                tool_render_set_mode(&renderer, TOOL_RENDER_DIFF);
            /* A hidden tail is model-only even when the tool never streamed. */
            size_t visible_len = strlen(output);
            if (run_ctx.output_hidden_tail <= visible_len)
                visible_len -= run_ctx.output_hidden_tail;
            tool_render_feed(&renderer, output, visible_len);
        }
    }
    tool_render_finalize(&renderer);
    spinner_hide(spinner);

    /* Use actual rendered rows: control stripping can erase otherwise non-empty display bytes. */
    if (tool_has_diff_output(tool) && renderer.display_was_called && renderer.rows_emitted == 0 &&
        output && *output)
        render_tool_solo_marker(render, output);

    struct item result = agent_tool_result_make(call, output, &run_ctx);
    free(output);
    tool_render_free(&renderer);
    return result;
}

struct item dispatch_tool_call(struct render_ctx *render, const struct item *call, int image_input)
{
    struct agent_tool_call prepared;
    agent_tool_call_init(&prepared, call);

    enum tool_preview_mode preview_mode = tool_call_preview_mode(&prepared.effective);
    render_set_mode(render,
                    preview_mode == TOOL_PREVIEW_COLLAPSED ? RENDER_TOOL_CLUSTER : RENDER_IDLE);
    struct item result =
        preview_mode == TOOL_PREVIEW_COLLAPSED
            ? dispatch_tool_call_collapsed(render, &prepared, image_input)
            : dispatch_tool_call_verbose(render, &prepared, preview_mode, image_input);
    agent_tool_call_destroy(&prepared);
    return result;
}
