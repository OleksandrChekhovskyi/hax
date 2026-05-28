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

/* Cells reserved at the right edge of a quiet line: the trailing space
 * before the spinner glyph (1) + the glyph itself (1) + breathing room
 * so the spinner doesn't crowd the right margin (~6). Pulled out here
 * so the silent-header sizing and coalesce-overflow check stay in
 * sync. */
#define QUIET_LINE_MARGIN 8

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
    disp_raw(ANSI_CYAN);
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
    /* Commit the trailing newline so the cursor is at column 0 of the
     * next line. The spinner shown during tool execution, or the tool
     * output itself, draws there instead of overwriting the header. */
    disp_emit_held(d);

    if (root)
        json_decref(root);
    fflush(stdout);
}

/* Silent-header writer for the start of a quiet line. For `read`,
 * `arg_text` is the file's basename (possibly with a `:N-M` slice
 * suffix); for quiet `bash`, it's the truncated command. The header is
 * NOT terminated with a newline — the caller decides whether to keep
 * the cursor parked at end-of-line for an inline spinner (reads, where
 * the next call may coalesce as `, baz.c`) or to emit `\n` and show a
 * labeled line-mode spinner below (non-coalescing kinds like bash).
 *
 * The whole header is dim — quiet calls are exploration breadcrumbs
 * that should recede visually, not compete with verbose tool blocks
 * (whose preview body is the focus) or model text. The cyan brackets
 * keep the tag scannable as a tool boundary even at lowered intensity.
 *
 * `reserve_spinner_space` adds a trailing space as breathing room for
 * the inline glyph; pass 0 when the caller will follow with `\n` so
 * redirected logs don't grow dangling spaces.
 *
 * Returns the visual byte cost of the line so far so the caller can
 * track when a coalesced line is about to overflow the terminal. */
static int write_silent_header(struct disp *d, const struct item *call, const char *arg_text,
                               int reserve_spinner_space)
{
    /* Visual budget tracking: we count what's *visible* — ANSI escapes
     * and the trailing space don't move the cursor visually, but the
     * tag, space, and arg do. Approximation; off-by-a-few is fine. */
    int used = 0;
    disp_raw(ANSI_DIM ANSI_CYAN);
    disp_printf(d, "[%s]", call->tool_name);
    /* Switch back to default foreground but keep DIM in effect so the
     * arg is dim too. ANSI_RESET would drop the dim attribute. */
    disp_raw(ANSI_FG_DEFAULT);
    used += 2 + (int)strlen(call->tool_name); /* "[name]" */
    disp_putc(d, ' ');
    used += 1;
    if (arg_text && *arg_text) {
        disp_write(d, arg_text, strlen(arg_text));
        used += (int)strlen(arg_text);
    }
    disp_raw(ANSI_RESET);
    /* Inline-spinner room: we need a space for the glyph, but only on
     * a TTY (no spinner otherwise — see write_silent_append's matching
     * tty check for why a non-TTY space would be a stray byte). */
    if (reserve_spinner_space && isatty(fileno(stdout))) {
        disp_putc(d, ' ');
        used += 1;
    }
    disp_emit_held(d); /* commit any held space so spinner lands inline */
    fflush(stdout);
    return used;
}

/* Silent-header append for read coalescing: writes ", basename" inline
 * onto the current line. Caller has already hidden the inline spinner
 * (which restored the cursor to the position right after the prior
 * filename's trailing space — disp_putc' space stays committed). */
static int write_silent_append(struct disp *d, const char *short_name)
{
    /* TTY: step back over the trailing space we left for the spinner
     * glyph, write ", short_name", then re-add the trailing space.
     * Cursor was at "...foo.c |" (| = cursor), after this we're at
     * "...foo.c, bar.c |". Backspace is safe since the line is
     * width-capped and we never wrap.
     *
     * Non-TTY: write_silent_header skipped the trailing space, so
     * there's nothing to step over — emitting `\b` would land a literal
     * control byte in the redirected log. Just append ", short_name". */
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
    /* Visible delta: ", " + name (+ restored space on TTY). The
     * backspace only undoes the space we'd previously laid down, which
     * we re-add at the end, so net is +2+strlen(name) regardless. */
    return 2 + (int)strlen(short_name);
}

/* Compute the arg text shown after the bracketed tag for a silent
 * call. Read uses basename of the file plus optional `:N-M`; bash uses
 * the command, truncated to fit in the available column budget.
 * `tag_cost` is the bytes consumed by `[name] ` so we know how many
 * columns are left for the arg. Returns malloc'd; caller frees. */
static char *make_silent_arg(const struct tool *tool, const struct item *call, int tag_cost,
                             int term_w)
{
    const char *name = call->tool_name;
    int budget = term_w - tag_cost - QUIET_LINE_MARGIN;
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
         * accepts e.g. `ls\npwd` as exploration) would otherwise wrap
         * the header across rows and put the inline spinner on the
         * wrong line. */
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

/* Silent dispatch: header-only, inline spinner, no preview. Coalesces
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

    /* Hide the cluster's own spinner and learn what mode it was in.
     * was_inline=1: cursor sits at the end of the prior cluster line
     * (we can append/coalesce). was_inline=0: prior spinner was line
     * mode (auto-transitioned, or labeled bash) OR there's no prior
     * spinner because this is the first call of a freshly-entered
     * cluster — cursor is at column 0 of a fresh row, and coalescing
     * isn't possible from there. */
    int was_inline = spinner_hide(sp);
    int can_coalesce = was_inline && r->cluster_last_tool &&
                       strcmp(r->cluster_last_tool, "read") == 0 &&
                       strcmp(call->tool_name, "read") == 0;
    /* Reads coalesce, so the inline glyph is parked at end-of-line as
     * an attachment point for a follow-up `, baz.c`. Bash never
     * coalesces — every silent bash gets its own header line — so go
     * straight to the labeled line-mode spinner below the header
     * instead of waiting for INLINE_TIMEOUT_MS to auto-transition.
     * Surfaces the "running..." label immediately for slow commands. */
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
        /* Flatten before measuring/appending so an embedded newline
         * in the path doesn't break the coalesced single-line header
         * (same reason as make_silent_arg's read branch). */
        char *append = flatten_for_display(full);
        free(full);
        size_t append_len = strlen(append);
        /* Cap coalesced line at ~term width so we never wrap. The
         * extra 2 covers ", " on top of the standard end-of-line
         * margin (trailing space + spinner + breathing room). */
        if (r->cluster_line_used + (int)append_len + 2 + QUIET_LINE_MARGIN > term_w) {
            /* Overflow → close current line, start a new `[read] …` header. */
            disp_putc(d, '\n');
            disp_emit_held(d);
            char *arg = make_silent_arg(t, call, 7 /* "[read] " */, term_w);
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
            /* Already inside the cluster but can't coalesce (different
             * silent kind, or coalesce-overflow): close the prior line
             * and write a fresh header below — but stay in RS_CLUSTER,
             * so no block separator between the lines. */
            if (was_inline) {
                disp_putc(d, '\n');
                disp_emit_held(d);
            } else {
                /* Prior spinner had auto-transitioned to line mode.
                 * spinner_hide cleared the row; cursor is at column 0
                 * of that fresh row. Sync trail so later separators
                 * compute correctly. */
                d->trail = 1;
            }
        }
        /* Else: first call after entering RS_CLUSTER — the transition
         * already left a clean column-0 row below prior content. */
        int tag_cost = 2 + (int)strlen(call->tool_name) + 1; /* "[name] " */
        char *arg = make_silent_arg(t, call, tag_cost, term_w);
        int used = write_silent_header(d, call, arg, inline_spinner);
        free(arg);
        r->cluster_line_used = used;
        if (!inline_spinner) {
            /* Close the header line so the line-mode spinner draws on
             * its own row below. Without this, the spinner thread's
             * `\r` + erase would clobber the [bash] header. */
            disp_putc(d, '\n');
            disp_emit_held(d);
        }
    }

    /* Bracket the run with "running..." / "working..." labels so the
     * spinner accurately reflects whether the tool is actively
     * executing. Silent path leaves the spinner visible after run()
     * to span the gap to the next call or stream event — without the
     * post-run reset, a stale "running..." would linger through that
     * gap (line-mode bash) or leak through an auto-transition (inline
     * read past INLINE_TIMEOUT_MS). The pre-run set ensures even
     * inline reads, if they auto-transition, surface the right label.
     * Set before show so the first frame draws with the new label
     * rather than the prior turn's residue. */
    spinner_set_label(sp, "running...");
    if (inline_spinner) {
        spinner_show_inline_header(sp);
    } else {
        spinner_show(sp);
    }

    /* Run the tool with no display callback — silent path discards live
     * stream and only keeps the canonical history. */
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
    /* "⠋ running..." — the spinner re-emerges between streamed chunks
     * (and for non-streaming tools, after the single returned payload),
     * so the user always sees "running" while a tool is executing. */
    spinner_set_label(sp, "running...");
    spinner_show(sp);

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
