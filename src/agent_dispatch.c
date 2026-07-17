/* SPDX-License-Identifier: MIT */
#include "agent_dispatch.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "tool.h"
#include "util.h"
#include "render/ctrl_strip.h"
#include "render/disp.h"
#include "render/spinner.h"
#include "render/tool_render.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Return a pointer into `path` at the basename — last component after
 * the final '/'. Trailing slashes (`src/`) fall back to the full path
 * since the basename would be empty. Returns "?" for NULL/empty. */
static const char *basename_view(const char *path)
{
    if (!path || !*path)
        return "?";
    const char *slash = strrchr(path, '/');
    if (!slash || slash[1] == '\0')
        return path;
    return slash + 1;
}

/* Decide whether this call should render with the silent-preview flow.
 * Static `silent_preview` flag wins when set; otherwise the optional
 * `is_silent` callback gets to inspect args (used by bash to classify
 * exploration commands at runtime). */
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

/* Hard cap on the dim suffix appended after the bold display_arg
 * (read's ":N-M" line range, etc.). The model controls the suffix
 * content via tool args; without a cap, an adversarial offset/limit
 * pair could produce a suffix that dominates the row. Real suffixes
 * are well under this. */
#define HEADER_EXTRA_CAP 20

/* Verbose tool-call header: block separator, `[name]` tag, the tool's
 * display_arg (full path / command), optional dim suffix. The arg is
 * word-wrapped across up to `tool->header_rows` rows (default 1) and
 * truncated with "..." beyond that, so it can't overflow into
 * terminal-driven mid-word wrapping. Terminated with `\n` and
 * committed so the spinner or output draws below. */
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
    disp_raw(theme_open(THEME_CHROME));
    disp_printf(d, "[%s]", call->tool_name);
    disp_raw(ANSI_RESET);

    int width = display_width();
    /* Tool names are ASCII so strlen == cells. */
    int prefix_cost = (int)strlen(call->tool_name) + 3; /* "[name] " */

    if (display_arg) {
        /* Reserve the dim extra suffix's cells out of the last row's
         * budget so the suffix stays attached without pushing the arg
         * over. format_display_extra is ASCII (e.g. read's ":30-50")
         * so byte length approximates cells. The model controls the
         * suffix content via tool args (a huge offset/limit pair on
         * read produces a 20+ char `:N-M`), so cap it here — without
         * a cap, a runaway suffix would dominate the row and shrink
         * the arg to "..." even when the arg is the user-interesting
         * part. */
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
        /* Generic JSON arg (for tools without a display_arg field): one
         * row, plain truncate. These are debug-grade — no point laying
         * out raw JSON across multiple rows. */
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
    /* Commit the trailing newline so the parked spinner and the tool
     * output draw below the header instead of overwriting it. */
    disp_emit_held(d);

    if (root)
        json_decref(root);
    fflush(stdout);
}

/* Silent-header writer for the start of a quiet line ("[read]
 * basename", "[bash] cmd"). NOT newline-terminated — the caller
 * leaves read lines open for coalescing and closes bash lines right
 * away. Quiet throughout so these calls recede as exploration
 * breadcrumbs; the chrome-styled brackets keep the tag scannable.
 *
 * Returns the line's cell count so far — exact cells, not bytes,
 * because it doubles as the parked spinner's cursor-restore column. */
static int write_silent_header(struct disp *d, const struct item *call, const char *arg_text)
{
    int used = 0;
    disp_raw(theme_open(THEME_CHROME_DIM));
    disp_printf(d, "[%s]", call->tool_name);
    /* The tag's quiet style comes from the role; the arg sits on the
     * default foreground, where SGR dim is portable, so re-open it
     * after the closer (which may clear intensity). */
    disp_raw(theme_close(THEME_CHROME_DIM));
    disp_raw(ANSI_DIM);
    used += 2 + (int)strlen(call->tool_name); /* "[name]", ASCII */
    disp_putc(d, ' ');
    used += 1;
    if (arg_text && *arg_text) {
        disp_write(d, arg_text, strlen(arg_text));
        used += (int)display_cells(arg_text);
    }
    disp_raw(ANSI_RESET);
    disp_emit_held(d);
    fflush(stdout);
    return used;
}

/* Silent-header append for read coalescing: writes ", basename" onto
 * the still-open cluster line. Caller has already hidden the parked
 * spinner, which restored the cursor to the end of the line. Returns
 * the appended cell count. */
static int write_silent_append(struct disp *d, const char *short_name)
{
    disp_raw(ANSI_DIM);
    disp_write(d, ", ", 2);
    disp_write(d, short_name, strlen(short_name));
    disp_raw(ANSI_RESET);
    disp_emit_held(d);
    fflush(stdout);
    return 2 + (int)display_cells(short_name);
}

/* Compute the arg text shown after the bracketed tag for a silent
 * call. Read uses basename of the file plus optional `:N-M`; bash uses
 * the command, truncated to fit in the available column budget.
 * `tag_cost` is the cells consumed by `[name] ` so we know how many
 * columns are left for the arg. One cell is always held back so the
 * line never reaches the physical last column (deferred-autowrap
 * guard). Returns malloc'd; caller frees. */
static char *make_silent_arg(const struct tool *tool, const struct item *call, int tag_cost,
                             int term_w)
{
    const char *name = call->tool_name;
    int budget = term_w - tag_cost - 1;
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
        /* Flatten before truncating: a model could send a path with
         * embedded newlines/control bytes; basename_view's split point
         * is the last `/` so any newline elsewhere comes through and
         * would break the silent header's single-line invariant. */
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
        /* Flatten before truncating: a multi-line command (bash_classify
         * accepts e.g. `ls\npwd` as exploration) would otherwise break
         * the silent header's single-line invariant. */
        char *flat = flatten_for_display(cmd ? cmd : "");
        char *trimmed = truncate_for_display(flat, (size_t)budget);
        free(flat);
        if (root)
            json_decref(root);
        return trimmed;
    }
    /* Generic fallback (no other tool currently goes silent). */
    return xstrdup("");
}

/* Render a synthesized "[interrupted]" block in place of running a tool,
 * and produce the matching tool_result item so the conversation stays
 * well-formed when Esc fires partway through a batch. */
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

/* Refuse a tool call without running it. Used in --raw mode: we
 * advertised no tools, so a tool_call from the provider is either a
 * model bug or a misbehaving backend — either way we MUST NOT execute
 * it. Display a refusal header for user visibility and feed an error
 * tool_result back so the conversation stays well-formed and the model
 * can recover (e.g. answer in plain text instead). */
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

/* Build the dim arg shown after the tag in a collapsed (replayed) tool
 * line: the tool's declared display_arg value (read/edit/write → path,
 * bash → command), plus read's ":N-M" range suffix when present,
 * flattened to one row and truncated to `budget` cells. Returns malloc'd
 * (caller frees) or NULL when there's nothing useful to show. */
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
    /* Quiet throughout so replayed calls recede as past context: the
     * role styles the tag, SGR dim (portable on the default foreground)
     * quiets the arg. */
    disp_raw(theme_open(THEME_CHROME_DIM));
    disp_printf(d, "[%s]", name);
    disp_raw(theme_close(THEME_CHROME_DIM));
    disp_raw(ANSI_DIM);

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
    /* Commit the line's newline (like display_tool_header) so the cursor
     * sits at column 0 of a fresh row and disp's trail accounting matches
     * the terminal — otherwise RS_CLUSTER's close-half assumes a fresh row
     * that isn't there and the next block's separator collapses to no gap. */
    disp_emit_held(d);
    fflush(stdout);
}

/* Silent dispatch: header-only, parked spinner, no preview. Coalesces
 * with the previous quiet line when the prior tool was the same kind
 * (currently only `read` chains visually). Output is captured for
 * conversation history exactly like the verbose path; only the live
 * display is suppressed.
 *
 * Caller (dispatch_tool_call) has already called render_transition
 * to RS_CLUSTER, so r->state is RS_CLUSTER and — on first entry —
 * a clean column-0 row is below whatever came before.
 * cluster_last_tool == NULL distinguishes "just entered the cluster"
 * from "continuing a cluster started by a previous call". */
static struct item dispatch_tool_call_silent(struct render_ctx *r, const struct item *call,
                                             const struct tool *t)
{
    struct disp *d = &r->disp;
    struct spinner *sp = r->spinner;
    int term_w = display_width();

    /* Hide the parked spinner from the previous call in this cluster
     * (no-op on first entry). For an open read line the hide restores
     * the cursor to the end of the line, ready to coalesce. */
    spinner_hide(sp);

    int is_read = strcmp(call->tool_name, "read") == 0;
    int can_coalesce = r->cluster_line_open && r->cluster_last_tool &&
                       strcmp(r->cluster_last_tool, "read") == 0 && is_read;

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
        /* Flatten before measuring/appending so an embedded newline
         * in the path doesn't break the coalesced single-line header
         * (same reason as make_silent_arg's read branch). */
        char *append = flatten_for_display(full);
        free(full);
        /* Cap the coalesced line one cell short of the terminal edge
         * so it never wraps (and the spinner's cursor-restore column
         * stays on this physical row). The 2 covers ", ". */
        if (r->cluster_line_used + 2 + (int)display_cells(append) > term_w - 1) {
            /* Overflow → close current line, start a new `[read] …` header. */
            disp_putc(d, '\n');
            disp_emit_held(d);
            char *arg = make_silent_arg(t, call, 7 /* "[read] " */, term_w);
            r->cluster_line_used = write_silent_header(d, call, arg);
            free(arg);
        } else {
            r->cluster_line_used += write_silent_append(d, append);
        }
        free(append);
        if (root)
            json_decref(root);
    } else {
        if (r->cluster_line_open) {
            /* Already inside the cluster but the open read line can't
             * absorb this call (different silent kind): close it and
             * write a fresh header below — but stay in RS_CLUSTER, so
             * no block separator between the lines. */
            disp_putc(d, '\n');
            disp_emit_held(d);
        }
        /* Else: first call after entering RS_CLUSTER — the transition
         * already left a clean column-0 row below prior content. */
        int tag_cost = 2 + (int)strlen(call->tool_name) + 1; /* "[name] " */
        char *arg = make_silent_arg(t, call, tag_cost, term_w);
        r->cluster_line_used = write_silent_header(d, call, arg);
        free(arg);
        if (!is_read) {
            /* Non-coalescing kinds never revisit their line — commit
             * its \n now so disp's trail stays truthful. */
            disp_putc(d, '\n');
            disp_emit_held(d);
        }
    }
    r->cluster_line_open = is_read;

    /* Park the spinner below the quiet line: at the read line's exact
     * end column (the next coalesce restores there), or column 0 of
     * the fresh row below a committed bash header. The run is
     * bracketed with "[tool] running..." / "working..." requests; the
     * settle window means only a genuinely slow tool surfaces its
     * name, and the spinner stays visible after run() to span the gap
     * to the next call or stream event. */
    char label[64], key[32];
    snprintf(label, sizeof(label), "[%s] running...", call->tool_name);
    snprintf(key, sizeof(key), "run:%s", call->tool_name);
    spinner_request_label(sp, key, label);
    spinner_park(sp, is_read ? r->cluster_line_used : 0);

    /* Run the tool with no display callback — silent path discards live
     * stream and only keeps the canonical history. */
    char *ret = t->run(call->tool_arguments_json, NULL, NULL);
    spinner_request_label(sp, "working", "working...");

    r->cluster_last_tool = t->def.name;

    char *history = ctrl_strip_dup(ret ? ret : "");
    free(ret);

    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = history,
    };
}

/* Render a single dim "solo" row (the "›" chevron block) carrying a terse
 * one-line tool outcome that bypasses the streaming preview — the
 * "(no changes)" marker and the created-summary fallback. `text` may be
 * model-controlled (the created summary embeds the file path), so flatten
 * it to one row and neutralize control bytes the same way the tool header
 * does, then truncate to the terminal width. */
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

/* Run one tool call: render the header, drive the renderer over either
 * streamed emit_display chunks or the canonical return value, and produce
 * the tool_result item that goes back to the model. The canonical history
 * is ctrl_stripped at this boundary so all tools' outputs land in the
 * conversation in the same normalized form; anything pushed through
 * emit_display is display-only and does not enter history. */
static struct item dispatch_tool_call_verbose(struct render_ctx *r, const struct item *call)
{
    struct disp *d = &r->disp;
    struct spinner *sp = r->spinner;
    display_tool_header(d, call);
    /* Neutral "working..." parked below the header — set immediately,
     * and deliberately not a "[name] running..." request: the header
     * one row up already names the tool, and a deferred request could
     * promote invisibly while the tool-status row owns the spinner and
     * then debut on a later block, claiming "running" below output
     * that is visibly finished. The immediate set also clears any
     * promoted composing label — the header is ground truth that
     * composing is over. The first output row unwinds the park, so the
     * block body lands tight against its header. */
    spinner_set_label(sp, "working", "working...");
    spinner_park(sp, 0);

    const struct tool *t = find_tool(call->tool_name);
    /* Initial mode comes straight from tool capability. For diff-capable
     * tools (write/edit) we leave R_DIFF for after run() — they never
     * stream, so we'll have the full return string in hand to check the
     * `--- ` prefix before deciding diff vs error preview. */
    enum render_mode mode = (t && t->preview_tail) ? R_HEAD_TAIL : R_HEAD_ONLY;
    struct tool_render rr;
    tool_render_init(&rr, d, sp, mode);
    char *ret = t ? t->run(call->tool_arguments_json, tool_render_emit, &rr)
                  : xasprintf("unknown tool: %s", call->tool_name);

    /* If the tool called emit_display at any point, the live preview is
     * already rendered. Otherwise feed the canonical return value through
     * once so the renderer treats both kinds uniformly. */
    if (ret && !rr.emit_called) {
        /* Empty output from a diff-capable tool means the write/edit
         * was a no-op (byte-identical content, see fs_write_with_diff).
         * Render the marker inline — feeding "" through the preview
         * renderer would leave the user staring at a bare tool header. */
        if (t && t->output_is_diff && !*ret) {
            emit_tool_solo_marker(d, sp, "(no changes)");
        } else {
            /* Diff-capable tools' success output starts with `--- `;
             * their failure output (error messages) doesn't. Switching
             * mode here keeps a botched write/edit flowing through the
             * standard preview path instead of mis-coloring it as a diff. */
            if (t && t->output_is_diff && strncmp(ret, "--- ", 4) == 0)
                rr.mode = R_DIFF;
            tool_render_feed(&rr, ret, strlen(ret));
        }
    }
    tool_render_finalize(&rr);
    /* Spinner may still be up (no-output case, or head-full resume that
     * we never hid in finalize). Belt-and-braces to make sure it's gone
     * before the next thing draws. */
    spinner_hide(sp);

    /* A diff-capable tool that streamed display content the renderer
     * elided to nothing — write creating a file whose content is only
     * whitespace, or control/escape bytes ctrl_strip drops — produces no
     * rows, leaving a bare header. Surface the canonical "created ..."
     * summary as a solo row so the block always has a body. Predicting
     * "would this render?" in the tool can't account for ctrl_strip
     * (an ANSI escape swallows its trailing bytes), so we decide here on
     * the actual row count. Only write reaches this: edit never streams,
     * and the no-emit branches above already render a row. */
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

    /* Optional per-tool arg normalization, applied once so the preview
     * and run() see the same rewritten payload. The model's original
     * args stay in `call` (and thus in conversation history); only the
     * local `effective` shadow item carries the rewrite. Field-by-field
     * copy is fine because we never free `effective` — its non-
     * rewritten fields still belong to `call`. */
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
