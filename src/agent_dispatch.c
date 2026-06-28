/* SPDX-License-Identifier: MIT */
#include "agent_dispatch.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent_core.h"
#include "tool.h"
#include "util.h"
#include "render/ctrl_strip.h"
#include "render/disp.h"
#include "render/spinner.h"
#include "render/tool_render.h"
#include "terminal/ansi.h"

/* Inline quiet spinners need: trailing space, glyph, phantom-wrap guard. */
#define INLINE_SPINNER_MARGIN 3

/* Pointer to basename; NULL/empty => "?", trailing slash => original path. */
static const char *basename_view(const char *path)
{
    if (!path || !*path)
        return "?";
    const char *slash = strrchr(path, '/');
    if (!slash || slash[1] == '\0')
        return path;
    return slash + 1;
}

/* Static silent_preview wins; bash can opt in per call via is_silent(args). */
static int call_is_silent(const struct tool *t, const struct item *call)
{
    if (!t)
        return 0;
    if (t->silent_preview)
        return 1;
    if (t->is_silent)
        return t->is_silent(call->tool_arguments_json);
    return 0;
}

/* Cap model-controlled dim suffixes (read's ":N-M" range, etc.). */
#define HEADER_EXTRA_CAP 20

/* Verbose header; commit it so spinner/output draws below. */
static void display_tool_header(struct disp *d, const struct item *call)
{
    const struct tool *tool = find_tool(call->tool_name);
    const char *display_arg = NULL;
    json_t *root = NULL;
    if (tool && tool->def.display_arg && call->tool_arguments_json) {
        json_error_t jerr;
        root = json_loads(call->tool_arguments_json, 0, &jerr);
        if (root)
            display_arg = json_string_value(json_object_get(root, tool->def.display_arg));
    }

    disp_block_separator(d);
    disp_raw(ANSI_CYAN);
    disp_printf(d, "[%s]", call->tool_name);
    disp_raw(ANSI_RESET);

    int width = display_width();
    /* Tool names are ASCII so strlen == cells. */
    int prefix_cost = (int)strlen(call->tool_name) + 3; /* "[name] " */

    if (display_arg) {
        /* Keep model-controlled suffixes from hiding the useful arg. */
        int extra_cost = 0;
        char *extra = NULL;
        if (tool && tool->format_display_extra) {
            extra = tool->format_display_extra(call->tool_arguments_json);
            if (extra && *extra) {
                int extra_cap = HEADER_EXTRA_CAP;
                if (extra_cap > width - 8)
                    extra_cap = width - 8;
                if (extra_cap < 4)
                    extra_cap = 4;
                if ((int)strlen(extra) > extra_cap) {
                    char *trimmed = truncate_for_display(extra, (size_t)extra_cap);
                    free(extra);
                    extra = trimmed;
                }
                extra_cost = (int)strlen(extra);
            }
        }

        int rows = tool && tool->header_rows > 0 ? tool->header_rows : 1;
        int first_row = width - prefix_cost;
        int mid_row = width;
        if (first_row < 8)
            first_row = 8;
        if (mid_row < 8)
            mid_row = 8;

        disp_putc(d, ' ');
        disp_raw(ANSI_BOLD);
        char *flat = flatten_for_display(display_arg);
        char *laid = reflow_for_display(flat, first_row, mid_row, rows, extra_cost);
        free(flat);
        disp_write(d, laid, strlen(laid));
        free(laid);
        disp_raw(ANSI_RESET);
        if (extra && *extra) {
            disp_raw(ANSI_DIM);
            disp_write(d, extra, strlen(extra));
            disp_raw(ANSI_RESET);
        }
        free(extra);
    } else if (call->tool_arguments_json && *call->tool_arguments_json) {
        disp_putc(d, ' ');
        disp_raw(ANSI_DIM);
        char *flat = flatten_for_display(call->tool_arguments_json);
        /* Generic JSON args are debug-grade: one row, plain truncate. */
        int budget = width - prefix_cost;
        if (budget < 8)
            budget = 8;
        char *trimmed = truncate_for_display(flat, (size_t)budget);
        disp_write(d, trimmed, strlen(trimmed));
        free(trimmed);
        free(flat);
        disp_raw(ANSI_RESET);
    }
    disp_putc(d, '\n');
    /* Commit the newline so spinner/output cannot overwrite the header. */
    disp_emit_held(d);

    if (root)
        json_decref(root);
    fflush(stdout);
}

/* Start a dim quiet-tool line; reads may park for coalescing/spinner. */
static int write_silent_header(struct disp *d, const struct item *call, const char *arg_text,
                               int reserve_spinner_space)
{
    /* Track visible width; ANSI escapes don't count. */
    int used = 0;
    disp_raw(ANSI_DIM ANSI_CYAN);
    disp_printf(d, "[%s]", call->tool_name);
    /* Drop cyan but keep DIM for the arg; RESET would clear both. */
    disp_raw(ANSI_FG_DEFAULT);
    used += 2 + (int)strlen(call->tool_name); /* "[name]" */
    disp_putc(d, ' ');
    used += 1;
    if (arg_text && *arg_text) {
        disp_write(d, arg_text, strlen(arg_text));
        used += (int)strlen(arg_text);
    }
    disp_raw(ANSI_RESET);
    /* Leave spinner space only on TTYs; otherwise it leaks into logs. */
    if (reserve_spinner_space && isatty(fileno(stdout))) {
        disp_putc(d, ' ');
        used += 1;
    }
    disp_emit_held(d); /* commit any held space so spinner lands inline */
    fflush(stdout);
    return used;
}

/* Append ", basename" to a read cluster after hiding the inline spinner. */
static int write_silent_append(struct disp *d, const char *short_name)
{
    /* TTY: replace reserved spinner space; non-TTY: avoid literal backspace in logs. */
    int tty = isatty(fileno(stdout));
    if (tty)
        fputs("\b", stdout);
    disp_raw(ANSI_DIM);
    disp_write(d, ", ", 2);
    disp_write(d, short_name, strlen(short_name));
    disp_raw(ANSI_RESET);
    if (tty)
        disp_putc(d, ' ');
    disp_emit_held(d);
    fflush(stdout);
    /* Net visible growth is only ", " + name; the TTY space is restored. */
    return 2 + (int)strlen(short_name);
}

/* Arg text for silent headers, truncated after tag/spinner reservations. */
static char *make_silent_arg(const struct tool *tool, const struct item *call, int tag_cost,
                             int term_w, int reserve_spinner_space)
{
    const char *name = call->tool_name;
    int margin = reserve_spinner_space ? INLINE_SPINNER_MARGIN : 1;
    int budget = term_w - tag_cost - margin;
    if (budget < 8)
        budget = 8;

    if (strcmp(name, "read") == 0) {
        const char *path = NULL;
        json_t *root = NULL;
        if (call->tool_arguments_json) {
            json_error_t jerr;
            root = json_loads(call->tool_arguments_json, 0, &jerr);
            if (root)
                path = json_string_value(json_object_get(root, "path"));
        }
        const char *base = basename_view(path);
        char *extra = NULL;
        if (tool && tool->format_display_extra)
            extra = tool->format_display_extra(call->tool_arguments_json);
        char *full = (extra && *extra) ? xasprintf("%s%s", base, extra) : xstrdup(base);
        free(extra);
        if (root)
            json_decref(root);
        /* Flatten before truncating; model-controlled paths may contain newlines. */
        char *flat = flatten_for_display(full);
        free(full);
        char *trimmed = truncate_for_display(flat, (size_t)budget);
        free(flat);
        return trimmed;
    }
    if (strcmp(name, "bash") == 0) {
        const char *cmd = NULL;
        json_t *root = NULL;
        if (call->tool_arguments_json) {
            json_error_t jerr;
            root = json_loads(call->tool_arguments_json, 0, &jerr);
            if (root)
                cmd = json_string_value(json_object_get(root, "command"));
        }
        /* Flatten before truncating; exploration commands may be multi-line. */
        char *flat = flatten_for_display(cmd ? cmd : "");
        char *trimmed = truncate_for_display(flat, (size_t)budget);
        free(flat);
        if (root)
            json_decref(root);
        return trimmed;
    }
    /* No other tool currently goes silent. */
    return xstrdup("");
}

/* Replace an unrun tool with [interrupted] and a matching tool_result. */
struct item dispatch_tool_skipped(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    display_tool_header(d, call);
    disp_tool_strip_solo(d);
    disp_raw(ANSI_DIM);
    disp_printf(d, "%s", INTERRUPT_MARKER);
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = xstrdup(INTERRUPT_MARKER),
    };
}

/* --raw advertised no tools; refuse backend tool_calls instead of executing. */
struct item dispatch_tool_refused(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    display_tool_header(d, call);
    disp_tool_strip_solo(d);
    disp_raw(ANSI_DIM);
    disp_printf(d, "[refused: --raw, no tools advertised]");
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = xstrdup("error: tool calls are disabled in this session"),
    };
}

/* Malloc'd collapsed header arg, or NULL when nothing useful fits. */
static char *collapsed_arg(const struct tool *t, const struct item *call, int budget)
{
    if (!t || !t->def.display_arg || !call->tool_arguments_json)
        return NULL;
    json_t *root = json_loads(call->tool_arguments_json, 0, NULL);
    const char *val = root ? json_string_value(json_object_get(root, t->def.display_arg)) : NULL;
    char *out = NULL;
    if (val) {
        char *extra =
            t->format_display_extra ? t->format_display_extra(call->tool_arguments_json) : NULL;
        char *full = (extra && *extra) ? xasprintf("%s%s", val, extra) : xstrdup(val);
        free(extra);
        char *flat = flatten_for_display(full);
        free(full);
        out = truncate_for_display(flat, (size_t)(budget < 8 ? 8 : budget));
        free(flat);
    }
    if (root)
        json_decref(root);
    return out;
}

void render_collapsed_tool_call(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    const char *name = call->tool_name ? call->tool_name : "?";
    /* Drop cyan but keep DIM for the arg; RESET would clear both. */
    disp_raw(ANSI_DIM ANSI_CYAN);
    disp_printf(d, "[%s]", name);
    disp_raw(ANSI_FG_DEFAULT);

    int tag_cost = 2 + (int)strlen(name) + 1; /* "[name] " */
    const struct tool *t = call->tool_name ? find_tool(call->tool_name) : NULL;
    char *arg = collapsed_arg(t, call, display_width() - tag_cost - 2);
    if (arg && *arg) {
        disp_putc(d, ' ');
        disp_write(d, arg, strlen(arg));
    }
    free(arg);
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    /* Sync disp's trail with the committed newline for later separators. */
    disp_emit_held(d);
    fflush(stdout);
}

/* Quiet dispatch: breadcrumbs/spinner only; still capture canonical output. */
static struct item dispatch_tool_call_silent(struct render_ctx *r, const struct item *call,
                                             const struct tool *t)
{
    struct disp *d = &r->disp;
    struct spinner *sp = r->spinner;
    int term_w = display_width();

    /* Inline spinner means the cursor is parked where read coalescing can append. */
    int was_inline = spinner_hide(sp);
    int can_coalesce = was_inline && r->cluster_last_tool &&
                       strcmp(r->cluster_last_tool, "read") == 0 &&
                       strcmp(call->tool_name, "read") == 0;
    /* Reads keep an inline spinner for coalescing; bash uses line-mode immediately. */
    int inline_spinner = strcmp(call->tool_name, "read") == 0;

    if (can_coalesce) {
        const char *path = NULL;
        json_t *root = NULL;
        if (call->tool_arguments_json) {
            json_error_t jerr;
            root = json_loads(call->tool_arguments_json, 0, &jerr);
            if (root)
                path = json_string_value(json_object_get(root, "path"));
        }
        const char *base = basename_view(path);
        char *extra = NULL;
        if (t && t->format_display_extra)
            extra = t->format_display_extra(call->tool_arguments_json);
        char *full = (extra && *extra) ? xasprintf("%s%s", base, extra) : xstrdup(base);
        free(extra);
        /* Preserve the coalesced header's single-line invariant. */
        char *append = flatten_for_display(full);
        free(full);
        size_t append_len = strlen(append);
        /* Cap at terminal width; +2 is the ", " append prefix. */
        if (r->cluster_line_used + (int)append_len + 2 + INLINE_SPINNER_MARGIN > term_w) {
            /* Overflow → close current line, start a new `[read] …` header. */
            disp_putc(d, '\n');
            disp_emit_held(d);
            char *arg = make_silent_arg(t, call, 7 /* "[read] " */, term_w,
                                        1 /* coalesce path is always read → inline spinner */);
            int used = write_silent_header(d, call, arg, 1 /* coalesce path is always read */);
            free(arg);
            r->cluster_line_used = used;
        } else {
            r->cluster_line_used += write_silent_append(d, append);
        }
        free(append);
        if (root)
            json_decref(root);
    } else {
        if (r->cluster_last_tool) {
            /* Same cluster, new line: no block separator between quiet calls. */
            if (was_inline) {
                disp_putc(d, '\n');
                disp_emit_held(d);
            } else {
                /* spinner_hide left a clean row; sync trail for later separators. */
                d->trail = 1;
            }
        }
        /* Else first RS_CLUSTER call already has a clean column-0 row. */
        int tag_cost = 2 + (int)strlen(call->tool_name) + 1; /* "[name] " */
        char *arg = make_silent_arg(t, call, tag_cost, term_w, inline_spinner);
        int used = write_silent_header(d, call, arg, inline_spinner);
        free(arg);
        r->cluster_line_used = used;
        if (!inline_spinner) {
            /* Put the line-mode spinner below the header so erase won't clobber it. */
            disp_putc(d, '\n');
            disp_emit_held(d);
        }
    }

    /* Keep the spinner between quiet calls; reset the label after run(). */
    spinner_set_label(sp, "running...");
    if (inline_spinner) {
        spinner_show_inline_header(sp);
    } else {
        spinner_show(sp);
    }

    /* No live preview on the silent path; keep only canonical history. */
    char *ret = t->run(call->tool_arguments_json, NULL, NULL);
    spinner_set_label(sp, "working...");

    r->cluster_last_tool = t->def.name;

    char *history = ctrl_strip_dup(ret ? ret : "");
    free(ret);

    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = history,
    };
}

/* One-line tool outcome that bypasses preview; flatten model-controlled text. */
static void emit_tool_solo_marker(struct disp *d, struct spinner *sp, const char *text)
{
    spinner_hide(sp);
    disp_tool_strip_solo(d);
    disp_raw(ANSI_DIM);
    int budget = display_width() - 2; /* "› " strip glyph + space */
    if (budget < 8)
        budget = 8;
    char *flat = flatten_for_display(text);
    char *line = truncate_for_display(flat, (size_t)budget);
    free(flat);
    disp_write(d, line, strlen(line));
    free(line);
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
}

/* Verbose dispatch: preview display output, return ctrl-stripped history. */
static struct item dispatch_tool_call_verbose(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    struct spinner *sp = r->spinner;
    display_tool_header(d, call);
    /* Spinner reappears between chunks while the tool is still running. */
    spinner_set_label(sp, "running...");
    spinner_show(sp);

    const struct tool *t = find_tool(call->tool_name);
    /* Diff-capable tools decide diff vs error preview after run(). */
    enum render_mode mode = (t && t->preview_tail) ? R_HEAD_TAIL : R_HEAD_ONLY;
    struct tool_render rr;
    tool_render_init(&rr, d, sp, mode);
    char *ret = t ? t->run(call->tool_arguments_json, tool_render_emit, &rr)
                  : xasprintf("unknown tool: %s", call->tool_name);

    /* Non-streaming tools feed their return value through the same renderer. */
    if (ret && !rr.emit_called) {
        /* Empty diff-capable output is a byte-identical no-op; show a body. */
        if (t && t->output_is_diff && !*ret) {
            emit_tool_solo_marker(d, sp, "(no changes)");
        } else {
            /* Only success output starts with `--- `; errors stay plain. */
            if (t && t->output_is_diff && strncmp(ret, "--- ", 4) == 0)
                rr.mode = R_DIFF;
            tool_render_feed(&rr, ret, strlen(ret));
        }
    }
    tool_render_finalize(&rr);
    /* Ensure no spinner survives a no-output/head-full finalize path. */
    spinner_hide(sp);

    /* If streamed diff preview elides every row, show the canonical summary. */
    if (t && t->output_is_diff && rr.emit_called && rr.rows_emitted == 0 && ret && *ret)
        emit_tool_solo_marker(d, sp, ret);

    char *history = ctrl_strip_dup(ret ? ret : "");
    free(ret);
    tool_render_free(&rr);

    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = history,
    };
}

struct item dispatch_tool_call(struct render_ctx *r, const struct item *call)
{
    const struct tool *t = find_tool(call->tool_name);

    /* Normalize args once for local preview/run; keep original args in history. */
    char *rewritten = NULL;
    if (t && t->preprocess_args && call->tool_arguments_json)
        rewritten = t->preprocess_args(call->tool_arguments_json);
    struct item effective = *call;
    if (rewritten)
        effective.tool_arguments_json = rewritten;
    const struct item *eff = rewritten ? &effective : call;

    int is_silent = t && call_is_silent(t, eff);
    render_transition(r, is_silent ? RS_CLUSTER : RS_IDLE);
    struct item out =
        is_silent ? dispatch_tool_call_silent(r, eff, t) : dispatch_tool_call_verbose(r, eff);
    free(rewritten);
    return out;
}
