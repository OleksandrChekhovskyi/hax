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
/* One character plus an ellipsis, so a suffix never squeezes the argument to a bare cut. */
#define MIN_ARGUMENT_CELLS 4

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

/* What follows the tool tag, resolved once per call so every header style consults the tool's
 * display hooks identically and lays the pieces out the same way. Members are owned. */
struct header_text {
    /* Flattened to one line; NULL when the call carried no arguments. */
    char *argument;
    /* Dim suffix the layout keeps intact under truncation; NULL when absent or for raw JSON. */
    char *extra;
    /* The argument is the JSON payload itself because no display hook derived one. */
    int raw;
};

/* Row cells left for content after the tag, floored so a narrow pane still shows something. */
static int row_budget(int cells)
{
    return cells < MIN_DISPLAY_CELLS ? MIN_DISPLAY_CELLS : cells;
}

/* Consumes the tool-formatted suffix; an empty one becomes NULL. The cap leaves the argument
 * its minimum share of the first row, so the row never exceeds its budget. */
static char *capped_extra(char *extra, int first_row_cells)
{
    if (!extra)
        return NULL;
    if (!*extra) {
        free(extra);
        return NULL;
    }
    int max_cells = HEADER_EXTRA_MAX_CELLS;
    if (max_cells > first_row_cells - MIN_ARGUMENT_CELLS)
        max_cells = first_row_cells - MIN_ARGUMENT_CELLS;
    if ((int)display_cells(extra) <= max_cells)
        return extra;
    char *trimmed = truncate_for_display(extra, (size_t)max_cells);
    free(extra);
    return trimmed;
}

/* Collapsed rows apply the tool's collapse rewrite so coalesced calls scan as a list.
 * first_row_cells is the budget the caller will lay the first row out with. */
static struct header_text header_text_resolve(const struct tool *tool, const struct item *call,
                                              int collapsed, int first_row_cells)
{
    struct header_text text = {0};
    const char *args_json = call->tool_arguments_json;
    char *argument = display_argument(tool, args_json);
    if (argument && collapsed && tool->display.collapse_argument) {
        char *rewritten = tool->display.collapse_argument(argument);
        free(argument);
        argument = rewritten;
    }
    if (argument) {
        if (tool->display.format_extra)
            text.extra = capped_extra(tool->display.format_extra(args_json), first_row_cells);
    } else {
        if (!args_json || !*args_json)
            return text;
        text.raw = 1;
        argument = xstrdup(args_json);
    }
    text.argument = flatten_for_display(argument);
    free(argument);
    return text;
}

static void header_text_free(struct header_text *text)
{
    free(text->argument);
    free(text->extra);
}

static int header_text_extra_cells(const struct header_text *text)
{
    return text->extra ? (int)display_cells(text->extra) : 0;
}

/* Fit the argument into the row budgets, leaving room for the suffix on whichever row ends up
 * last. Caller frees. */
static char *header_text_layout(const struct header_text *text, int first_row_cells,
                                int other_row_cells, int max_rows)
{
    return reflow_for_display(text->argument, first_row_cells, other_row_cells, max_rows,
                              header_text_extra_cells(text));
}

static void write_header_extra(struct disp *disp, const struct header_text *text)
{
    if (!text->extra)
        return;
    disp_write_ansi(disp, ANSI_DIM);
    disp_write(disp, text->extra, strlen(text->extra));
    disp_write_ansi(disp, ANSI_RESET);
}

static void write_tool_header(struct disp *disp, const struct item *call)
{
    const char *name = tool_name(call);
    const struct tool *tool = agent_find_tool(name);
    int terminal_width = display_width();
    int first_row_cells = row_budget(terminal_width - tool_tag_cells(name));
    struct header_text text = header_text_resolve(tool, call, 0, first_row_cells);

    disp_block_separator(disp);
    disp_write_ansi(disp, theme_open(THEME_CHROME));
    disp_printf(disp, "[%s]", name);
    disp_write_ansi(disp, ANSI_RESET);

    if (text.argument) {
        int rows = !text.raw && tool->display.header_rows > 0 ? tool->display.header_rows : 1;
        char *layout = header_text_layout(&text, first_row_cells, row_budget(terminal_width), rows);
        disp_putc(disp, ' ');
        disp_write_ansi(disp, text.raw ? ANSI_DIM : ANSI_BOLD);
        disp_write(disp, layout, strlen(layout));
        disp_write_ansi(disp, ANSI_RESET);
        free(layout);
        write_header_extra(disp, &text);
    }
    header_text_free(&text);

    disp_putc(disp, '\n');
    /* Commit the newline before the spinner or output can overprint it. */
    disp_commit_newlines(disp);
    disp_flush(disp);
}

/* Read headers remain open so later reads can coalesce on the same row. Returns the cells the
 * row occupies. */
static int write_collapsed_header(struct disp *disp, const struct item *call,
                                  const struct header_text *text, int row_cells)
{
    const char *name = tool_name(call);
    int cells = tool_tag_cells(name);
    char *layout = header_text_layout(text, row_cells, row_cells, 1);

    disp_write_ansi(disp, theme_open(THEME_CHROME_DIM));
    disp_printf(disp, "[%s]", name);
    /* The theme closer may clear intensity, so restore dim for the argument. */
    disp_write_ansi(disp, theme_close(THEME_CHROME_DIM));
    disp_write_ansi(disp, ANSI_DIM);
    disp_putc(disp, ' ');
    disp_write(disp, layout, strlen(layout));
    cells += (int)display_cells(layout);
    disp_write_ansi(disp, ANSI_RESET);
    free(layout);
    write_header_extra(disp, text);
    cells += header_text_extra_cells(text);
    disp_commit_newlines(disp);
    disp_flush(disp);
    return cells;
}

/* Cells a call appended to an open row would occupy. */
static int coalesced_cells(const struct header_text *text)
{
    return 2 + (int)display_cells(text->argument) + header_text_extra_cells(text);
}

static int append_collapsed_argument(struct disp *disp, const struct header_text *text)
{
    disp_write_ansi(disp, ANSI_DIM);
    disp_write(disp, ", ", 2);
    if (text->argument)
        disp_write(disp, text->argument, strlen(text->argument));
    disp_write_ansi(disp, ANSI_RESET);
    write_header_extra(disp, text);
    disp_commit_newlines(disp);
    disp_flush(disp);
    return coalesced_cells(text);
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

    /* Keep one cell free to avoid deferred terminal autowrap on an open row. */
    int row_cells = row_budget(terminal_width - tool_tag_cells(tool_name(call)) - 1);
    struct header_text text = header_text_resolve(tool, call, 1, row_cells);
    if (can_coalesce) {
        if (render->cluster.line_cells + coalesced_cells(&text) > terminal_width - 1) {
            close_collapsed_line(disp);
            render->cluster.line_cells = write_collapsed_header(disp, call, &text, row_cells);
        } else {
            render->cluster.line_cells += append_collapsed_argument(disp, &text);
        }
    } else {
        if (render->cluster.line_open)
            close_collapsed_line(disp);
        render->cluster.line_cells = write_collapsed_header(disp, call, &text, row_cells);
        if (!is_read)
            close_collapsed_line(disp);
    }
    header_text_free(&text);
    render->cluster.line_open = is_read;
    render->cluster.last_tool = tool ? tool->def.name : NULL;
}

void render_collapsed_tool_call(struct render_ctx *render, const struct item *call)
{
    write_cluster_line(render, call);
}

static struct item dispatch_tool_call_collapsed(struct render_ctx *render,
                                                struct agent_tool_call *prepared, int image_input,
                                                void *user)
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

    struct tool_run_ctx run_ctx = {.image_input = image_input, .user = user};
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
                                              enum tool_preview_mode preview_mode, int image_input,
                                              void *user)
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
        .user = user,
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

struct item dispatch_tool_call(struct render_ctx *render, const struct item *call, int image_input,
                               void *user)
{
    struct agent_tool_call prepared;
    agent_tool_call_init(&prepared, call);

    enum tool_preview_mode preview_mode = tool_call_preview_mode(&prepared.effective);
    render_set_mode(render,
                    preview_mode == TOOL_PREVIEW_COLLAPSED ? RENDER_TOOL_CLUSTER : RENDER_IDLE);
    struct item result =
        preview_mode == TOOL_PREVIEW_COLLAPSED
            ? dispatch_tool_call_collapsed(render, &prepared, image_input, user)
            : dispatch_tool_call_verbose(render, &prepared, preview_mode, image_input, user);
    agent_tool_call_destroy(&prepared);
    return result;
}
