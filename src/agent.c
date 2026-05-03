/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "agent_core.h"
#include "ansi.h"
#include "ctrl_strip.h"
#include "disp.h"
#include "input.h"
#include "interrupt.h"
#include "markdown.h"
#include "slash.h"
#include "spawn.h"
#include "spinner.h"
#include "tool.h"
#include "tool_render.h"
#include "transcript.h"
#include "turn.h"
#include "util.h"

#define INTERRUPT_MARKER "[interrupted]"

/* The ASCII fallback is used when locale_init_utf8() couldn't establish a
 * UTF-8 LC_CTYPE — wcwidth() under a non-UTF-8 locale would mis-account
 * the multibyte glyph and break cursor positioning. */
#define PROMPT_UTF8  ANSI_MAGENTA ANSI_BOLD "❯" ANSI_BOLD_OFF ANSI_FG_DEFAULT " "
#define PROMPT_ASCII ANSI_BOLD ">" ANSI_BOLD_OFF " "

/* True when `it` is a tool_result whose output already ends with the
 * interrupt marker — i.e. bash appended its "[interrupted]" footer when
 * killed by user-Esc. Used to decide whether the post-dispatch break
 * needs an additional synthetic marker, since non-bash tools (read,
 * write, edit) finish normally without any footer and would otherwise
 * leave the next turn with no signal that the user aborted. */
static int tool_result_is_marked(const struct item *it)
{
    if (it->kind != ITEM_TOOL_RESULT || !it->output)
        return 0;
    size_t out_len = strlen(it->output);
    size_t marker_len = strlen(INTERRUPT_MARKER);
    if (out_len < marker_len)
        return 0;
    return strcmp(it->output + out_len - marker_len, INTERRUPT_MARKER) == 0;
}

/* Tracks an in-progress run of "quiet" tool calls — read/list/grep-style
 * exploration whose output is hidden from display. The cluster collapses
 * the visual block separator between consecutive quiet calls, and
 * coalesces consecutive `read` calls onto one line: `[read] foo.c, bar.c,
 * baz.c`. The inline spinner sits at the end of the active line as
 * "still alive" indicator, surviving across both tool-runs and the wait
 * for the next provider event.
 *
 * `active` means a quiet line is currently on the terminal with no
 * trailing newline (and the inline spinner may be drawn). `last_tool` is
 * the name of the most recent quiet tool, used to decide whether the
 * next call coalesces. `line_used` is a byte budget for the current
 * line, checked against the terminal width before appending another
 * filename. */
struct cluster {
    int active;
    const char *last_tool; /* borrowed from struct tool's static name */
    int line_used;
};

/* Bundles the per-turn state needed by the streaming callback into one
 * struct, so we can plumb display state alongside `struct turn` through
 * the provider's user-pointer parameter. */
struct event_ctx {
    struct disp *disp;
    struct turn *turn;
    struct spinner *spinner;
    struct md_renderer *md; /* NULL when markdown is disabled */
    struct cluster *cl;
    /* Filled in from EV_DONE; -1/-1/-1 if the provider didn't report. */
    struct stream_usage usage;
};

/* Cells reserved at the right edge of a quiet line: the trailing space
 * before the spinner glyph (1) + the glyph itself (1) + breathing room
 * so the spinner doesn't crowd the right margin (~6). Pulled out here
 * so the silent-header sizing and coalesce-overflow check stay in
 * sync. */
#define QUIET_LINE_MARGIN 8

/* Query the host terminal width via TIOCGWINSZ, lazily on each call so
 * SIGWINCH-style resizes are picked up without explicit handling. Falls
 * back to 120 cols when stdout isn't a TTY or the ioctl fails. The
 * returned value is clamped so silent headers stay readable on narrow
 * terminals (40) and don't get pathologically long on wide ones (200). */
static int term_width(void)
{
    struct winsize ws;
    int w = 120;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        w = ws.ws_col;
    if (w < 40)
        w = 40;
    if (w > 200)
        w = 200;
    return w;
}

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

/* Truncate a UTF-8 string to fit in `cap` bytes, replacing the cut
 * suffix with "..." so the user sees an explicit "more here" marker.
 * Same convention as the elision markers in tool_render.c / bash.c so
 * the truncation idiom reads consistently across the UI. Walks back
 * from the cut point to a UTF-8 codepoint boundary so the replacement
 * doesn't leave a half-encoded byte. Returns malloc'd. */
static char *truncate_for_display(const char *s, size_t cap)
{
    size_t n = strlen(s);
    if (n <= cap)
        return xstrdup(s);
    if (cap < 4) {
        char *out = xmalloc(cap + 1);
        memcpy(out, s, cap);
        out[cap] = '\0';
        return out;
    }
    size_t cut = cap - 3;
    while (cut > 0 && (s[cut] & 0xC0) == 0x80)
        cut--;
    char *out = xmalloc(cut + 4);
    memcpy(out, s, cut);
    memcpy(out + cut, "...", 3);
    out[cut + 3] = '\0';
    return out;
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

/* Terminate the in-progress quiet line and reset cluster state.
 * Idempotent on inactive cluster.
 *
 * Two cases depending on what the spinner was doing at hide time:
 *   - Inline glyph: hide restored the cursor to the end of the cluster
 *     line (after our trailing space). Emit `\n` to close the line —
 *     it goes into disp's held buffer so a following block_separator
 *     can collapse, or a following content write commits it.
 *   - Line spinner (auto-transitioned): hide erased the spinner's row,
 *     leaving the cursor at column 0 of an already-fresh line. The
 *     transition's own `\n` is on the terminal already but bypassed
 *     disp's tracking, so bump trail to 1 here to keep a following
 *     block_separator emitting the right number of newlines. */
static void cluster_terminate(struct cluster *cl, struct disp *d, struct spinner *sp)
{
    if (!cl->active)
        return;
    int was_inline = spinner_hide(sp);
    if (was_inline) {
        disp_putc(d, '\n');
    } else {
        d->trail = 1;
    }
    fflush(stdout);
    cl->active = 0;
    cl->last_tool = NULL;
    cl->line_used = 0;
}

/* Verbose tool-call header: block separator, `[name]` tag, the tool's
 * display_arg (full path / command), optional dim suffix. Terminated
 * with `\n` and committed so the spinner or output draws below. */
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

    if (display_arg) {
        disp_putc(d, ' ');
        disp_raw(ANSI_BOLD);
        char *flat = flatten_for_display(display_arg);
        disp_write(d, flat, strlen(flat));
        free(flat);
        disp_raw(ANSI_RESET);
        if (tool && tool->format_display_extra) {
            char *extra = tool->format_display_extra(call->tool_arguments_json);
            if (extra && *extra) {
                disp_raw(ANSI_DIM);
                disp_write(d, extra, strlen(extra));
                disp_raw(ANSI_RESET);
            }
            free(extra);
        }
    } else if (call->tool_arguments_json && *call->tool_arguments_json) {
        disp_putc(d, ' ');
        disp_raw(ANSI_DIM);
        char *flat = flatten_for_display(call->tool_arguments_json);
        disp_write(d, flat, strlen(flat));
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
 * NOT terminated with a newline — the inline spinner sits at the
 * cursor's resting position and is animated until either the next
 * silent call extends the line or cluster_terminate ends it.
 *
 * The whole header is dim — quiet calls are exploration breadcrumbs
 * that should recede visually, not compete with verbose tool blocks
 * (whose preview body is the focus) or model text. The cyan brackets
 * keep the tag scannable as a tool boundary even at lowered intensity.
 *
 * Returns the visual byte cost of the line so far so the caller can
 * track when a coalesced line is about to overflow the terminal. */
static int write_silent_header(struct disp *d, const struct item *call, const char *arg_text)
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
    /* Trailing space exists only as breathing room for the inline
     * spinner glyph. When stdout isn't a TTY we don't draw the spinner
     * (and don't backspace over the space during coalescing — see
     * write_silent_append), so skip it to keep redirected logs free of
     * dangling spaces. */
    if (isatty(fileno(stdout))) {
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
        /* Flatten before truncating: a multi-line command (cmd_classify
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
static struct item dispatch_tool_skipped(struct disp *d, const struct item *call)
{
    display_tool_header(d, call);
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
static struct item dispatch_tool_refused(struct disp *d, const struct item *call)
{
    display_tool_header(d, call);
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
 * display is suppressed. */
static struct item dispatch_tool_call_silent(struct cluster *cl, struct disp *d, struct spinner *sp,
                                             const struct item *call, const struct tool *t)
{
    int term_w = term_width();

    /* Hide whichever mode the spinner is in and learn what mode was
     * active at hide time. was_inline=1 means the cursor sits at the
     * end of the prior cluster line (we can append/coalesce); =0 means
     * the spinner had auto-transitioned to a labeled line below the
     * cluster, and the cursor is now at column 0 of that just-erased
     * row — coalescing isn't possible from there. */
    int was_inline = spinner_hide(sp);
    int can_coalesce = cl->active && was_inline && cl->last_tool &&
                       strcmp(cl->last_tool, "read") == 0 && strcmp(call->tool_name, "read") == 0;

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
        if (cl->line_used + (int)append_len + 2 + QUIET_LINE_MARGIN > term_w) {
            /* Overflow → close current line, start a new `[read] …` header. */
            disp_putc(d, '\n');
            disp_emit_held(d);
            char *arg = make_silent_arg(t, call, 7 /* "[read] " */, term_w);
            int used = write_silent_header(d, call, arg);
            free(arg);
            cl->line_used = used;
        } else {
            cl->line_used += write_silent_append(d, append);
        }
        free(append);
        if (root)
            json_decref(root);
    } else {
        if (cl->active) {
            if (was_inline) {
                /* Different silent kind (read → bash exploration, or
                 * vice versa). Close the prior inline line but stay
                 * inside the cluster — no block separator. */
                disp_putc(d, '\n');
                disp_emit_held(d);
            } else {
                /* Spinner auto-transitioned. The transition's \n is
                 * already on screen and spinner_hide cleared the
                 * spinner row; we're at column 0 of that empty row.
                 * Sync disp.trail so any later block_separator collapses
                 * correctly. */
                d->trail = 1;
            }
        } else {
            /* First quiet call: separate from whatever came before. */
            disp_block_separator(d);
        }
        int tag_cost = 2 + (int)strlen(call->tool_name) + 1; /* "[name] " */
        char *arg = make_silent_arg(t, call, tag_cost, term_w);
        int used = write_silent_header(d, call, arg);
        free(arg);
        cl->line_used = used;
    }

    spinner_show_inline(sp);

    /* Run the tool with no display callback — silent path discards live
     * stream and only keeps the canonical history. */
    char *ret = t->run(call->tool_arguments_json, NULL, NULL);

    cl->active = 1;
    cl->last_tool = t->def.name;

    char *history = ctrl_strip_dup(ret ? ret : "");
    free(ret);

    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = history,
    };
}

/* Run one tool call: render the header, drive the renderer over either
 * streamed emit_display chunks or the canonical return value, and produce
 * the tool_result item that goes back to the model. The canonical history
 * is ctrl_stripped at this boundary so all tools' outputs land in the
 * conversation in the same normalized form; anything pushed through
 * emit_display is display-only and does not enter history. */
static struct item dispatch_tool_call_verbose(struct disp *d, struct spinner *sp,
                                              const struct item *call)
{
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
            spinner_hide(sp);
            disp_raw(ANSI_DIM);
            disp_printf(d, "(no changes)");
            disp_raw(ANSI_RESET);
            disp_putc(d, '\n');
            fflush(stdout);
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

    char *history = ctrl_strip_dup(ret ? ret : "");
    free(ret);
    tool_render_free(&rr);

    return (struct item){
        .kind = ITEM_TOOL_RESULT,
        .call_id = xstrdup(call->call_id),
        .output = history,
    };
}

/* Top-level dispatch: pick silent or verbose path based on the tool's
 * static silent_preview flag and (for bash) per-call classification of
 * the command. Falling into the verbose path always terminates any
 * active quiet cluster first so the visual block separator reappears. */
static struct item dispatch_tool_call(struct cluster *cl, struct disp *d, struct spinner *sp,
                                      const struct item *call)
{
    const struct tool *t = find_tool(call->tool_name);
    if (t && call_is_silent(t, call))
        return dispatch_tool_call_silent(cl, d, sp, call, t);
    cluster_terminate(cl, d, sp);
    return dispatch_tool_call_verbose(d, sp, call);
}

/* Adapter so md_renderer can emit through disp without knowing about it.
 * Content bytes go through disp_write so trailing-newline buffering works
 * uniformly. ANSI escapes (is_raw=1) bypass disp's trail/held bookkeeping
 * — otherwise an escape after a buffered \n would commit the held NL and
 * reset trail to 0, leaking an extra blank line out of the next
 * disp_block_separator. The escape goes ahead of the held NL on stdout
 * but is zero-width, so the visible result is identical. */
static void md_emit_to_disp(const char *bytes, size_t n, int is_raw, void *user)
{
    if (is_raw)
        fwrite(bytes, 1, n, stdout);
    else
        disp_write((struct disp *)user, bytes, n);
}

static int markdown_enabled(void)
{
    if (!isatty(fileno(stdout)))
        return 0;
    const char *e = getenv("HAX_MARKDOWN");
    if (e && strcmp(e, "0") == 0)
        return 0;
    return 1;
}

/* Format a token count in 1024-base: "412", "5.4k", "128k", "1.2M".
 * Powers-of-two are used because advertised context windows are typically
 * 32k/128k/256k (= 32×1024 etc.), so 1024-base produces the cleaner round
 * numbers users expect. < 0 → "?" (provider didn't report). */
static void format_tokens(char *buf, size_t buflen, long n)
{
    if (n < 0)
        snprintf(buf, buflen, "?");
    else if (n < 1024)
        snprintf(buf, buflen, "%ld", n);
    else if (n < 10L * 1024)
        snprintf(buf, buflen, "%.1fk", (double)n / 1024.0);
    else if (n < 1024L * 1024)
        snprintf(buf, buflen, "%ldk", (n + 512) / 1024);
    else if (n < 10L * 1024 * 1024)
        snprintf(buf, buflen, "%.1fM", (double)n / (1024.0 * 1024.0));
    else
        snprintf(buf, buflen, "%ldM", (n + 512L * 1024) / (1024L * 1024));
}

/* Parse a size with optional k/m suffix (case-insensitive, 1024-base):
 * "256k" → 262144, "128K" → 131072, "1m" → 1048576, "4096" → 4096.
 * Returns 0 on empty/invalid input. */
static long parse_size(const char *s)
{
    if (!s || !*s)
        return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || v <= 0)
        return 0;
    while (*end == ' ' || *end == '\t')
        end++;
    switch (*end) {
    case 'k':
    case 'K':
        v *= 1024L;
        end++;
        break;
    case 'm':
    case 'M':
        v *= 1024L * 1024L;
        end++;
        break;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return 0;
    return v;
}

/* Optional override: HAX_CONTEXT_LIMIT lets the user supply the model's
 * context window (e.g. "256k") so we can show a percentage. There's no
 * reliable way to auto-detect this across OpenAI-compatible local servers
 * (Ollama, llama.cpp, vLLM, oMLX, LM Studio all expose it differently or
 * not at all), so we ask. Returns 0 when unset/invalid → percentage is
 * hidden. */
static long context_limit(void)
{
    return parse_size(getenv("HAX_CONTEXT_LIMIT"));
}

/* Dim one-liner: "context 8.9k / 256k (3%) · out 595 · cached 2.7k", shown
 * once per user turn so multi-step tool runs collapse into a single summary
 * instead of bracketing every intermediate response.
 *
 * ctx and cached reflect the last response (= current window state — each
 * call's input subsumes the prior call's prefix, so the latest values are
 * the right snapshot). out is a running sum across the turn's model calls,
 * answering "how many tokens did this prompt cost in generation". Each
 * value is -1 when the underlying counts weren't reported by the backend;
 * the section is then skipped rather than rendered with a misleading zero. */
static void display_usage(struct disp *d, struct cluster *cl, struct spinner *sp, long ctx,
                          long out, long cached)
{
    int show_ctx = ctx >= 0;
    int show_out = out >= 0;
    int show_cached = cached > 0;
    if (!show_ctx && !show_out && !show_cached)
        return;

    cluster_terminate(cl, d, sp);
    disp_block_separator(d);
    disp_raw(ANSI_DIM);

    const char *sep = "";
    char buf[32], limit_buf[32];
    if (show_ctx) {
        format_tokens(buf, sizeof(buf), ctx);
        disp_printf(d, "context %s", buf);
        long limit = context_limit();
        if (limit > 0) {
            format_tokens(limit_buf, sizeof(limit_buf), limit);
            disp_printf(d, " / %s (%ld%%)", limit_buf, ctx * 100 / limit);
        }
        sep = " · ";
    }
    if (show_out) {
        format_tokens(buf, sizeof(buf), out);
        disp_printf(d, "%sout %s", sep, buf);
        sep = " · ";
    }
    if (show_cached) {
        format_tokens(buf, sizeof(buf), cached);
        disp_printf(d, "%scached %s", sep, buf);
    }
    disp_raw(ANSI_RESET);
    disp_putc(d, '\n');
    fflush(stdout);
}

static int on_event(const struct stream_event *ev, void *user)
{
    struct event_ctx *ec = user;
    struct disp *d = ec->disp;

    switch (ev->kind) {
    case EV_TEXT_DELTA: {
        /* Peek-strip first so we keep the spinner up across deltas that
         * are entirely leading newlines — flickering it off without any
         * output to take its place would reintroduce a silent wait. */
        const char *s = ev->u.text_delta.text;
        size_t n = strlen(s);
        disp_first_delta_strip(d, &s, &n);
        if (n == 0)
            break;
        /* Hide first so a stale "thinking..." label isn't briefly
         * repainted on its way out, then restore the default — visible
         * text means reasoning is over, and any later show in this turn
         * (e.g. before tool dispatch) should read "working...". */
        if (!d->saw_text) {
            /* Cluster termination must precede the spinner_hide below
             * so cluster_terminate observes the real prior spinner
             * mode and picks the right disp.trail accounting
             * (inline → close line with \n; line → trail=1 since the
             * spinner already lived on its own row). */
            cluster_terminate(ec->cl, d, ec->spinner);
            disp_block_separator(d);
            d->saw_text = 1;
        }
        spinner_hide(ec->spinner);
        spinner_set_label(ec->spinner, "working...");
        if (ec->md)
            md_feed(ec->md, s, n);
        else
            disp_write(d, s, n);
        fflush(stdout);
        break;
    }
    case EV_TOOL_CALL_START:
    case EV_REASONING_ITEM:
        /* Reasoning has ended — restore the default label so a stale
         * "thinking..." doesn't linger while the model emits tool-call
         * arguments (especially visible with reasoning models that emit
         * large write/edit args after CoT) or while we wait for the
         * next event after a Codex reasoning round-trip blob. The
         * tool-call's own header + output render as one block at
         * dispatch time, so we don't draw anything live here. */
        spinner_set_label(ec->spinner, "working...");
        break;
    case EV_TOOL_CALL_DELTA:
    case EV_TOOL_CALL_END:
        /* No live display: tool calls render as a single block during
         * dispatch so parallel calls don't visually interleave. */
        break;
    case EV_REASONING_DELTA:
        /* Activity-only signal: flip the spinner to "thinking..." so the
         * user can tell a long quiet pause is the model reasoning, not
         * the network. Restored to "working..." on the first EV_TEXT_DELTA,
         * EV_TOOL_CALL_START, or EV_REASONING_ITEM — whichever ends CoT. */
        spinner_set_label(ec->spinner, "thinking...");
        break;
    case EV_DONE:
        ec->usage = ev->u.done.usage;
        fflush(stdout);
        break;
    case EV_ERROR:
        /* Same ordering rule as EV_TEXT_DELTA: cluster_terminate first
         * so it observes the real prior spinner mode. */
        cluster_terminate(ec->cl, d, ec->spinner);
        spinner_hide(ec->spinner);
        /* Flush any pending markdown tail/styles so the model's last
         * bytes appear with the model text, not after the error block.
         * Idempotent — the post-stream md_flush in agent_run is a no-op
         * after this. */
        if (ec->md)
            md_flush(ec->md);
        disp_block_separator(d);
        disp_raw(ANSI_RED);
        disp_printf(d, "[error: %s]", ev->u.error.message);
        disp_raw(ANSI_RESET);
        disp_putc(d, '\n');
        fflush(stdout);
        break;
    }

    turn_on_event(ev, ec->turn);
    return 0;
}

/* Indirect references into the live agent_session so the Ctrl-T
 * callback always sees the latest values — `items` is reassigned by
 * xrealloc as the vector grows, and `n_items` advances with every
 * append. Holding the addresses sidesteps the moving target. `sys` is
 * fixed for the session's lifetime, but routing it through the same
 * indirection keeps the three fields uniform.
 *
 * Lifetime: instances live on agent_run's stack frame and are
 * registered with the input editor for the duration of that call.
 * input_free must run before agent_run returns (it does — see the
 * cleanup at the end of agent_run), otherwise `input` would outlive
 * the storage and a stray Ctrl-T would dereference a dead frame. */
struct transcript_view {
    const char *const *sys_ref;
    struct item *const *items_ref;
    const size_t *n_items_ref;
};

static void show_transcript_cb(void *user)
{
    struct transcript_view *v = user;
    const char *pager = getenv("PAGER");
    if (!pager || !*pager)
        pager = "less -R";
    /* spawn_pipe_open shields the parent from terminal-generated
     * SIGINT/SIGQUIT (so Ctrl-C in the pager exits the pager, not
     * hax) and from SIGPIPE on the fputs path (so quitting the pager
     * early gives EPIPE rather than killing hax). The child sees all
     * three at default disposition, so less behaves normally. */
    struct spawn_pipe sp;
    if (spawn_pipe_open(&sp, pager) < 0)
        return;
    transcript_render(sp.w, *v->sys_ref, *v->items_ref, *v->n_items_ref);
    spawn_pipe_close(&sp);
}

void agent_print_banner(const struct provider *p, const struct agent_session *s)
{
    const char *bar = ANSI_CYAN "▌" ANSI_FG_DEFAULT;
    if (s->reasoning_effort)
        printf("\n%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s · %s" ANSI_BOLD_OFF
               "\n",
               bar, p->name ? p->name : "?", s->model, s->reasoning_effort);
    else
        printf("\n%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s" ANSI_BOLD_OFF "\n",
               bar, p->name ? p->name : "?", s->model);
    printf("%s " ANSI_DIM "ctrl-d quit · try /help" ANSI_BOLD_OFF "\n", bar);
}

int agent_run(struct provider *p, const struct hax_opts *opts)
{
    struct agent_session sess;
    if (agent_session_init(&sess, p, opts) < 0)
        return 1;

    agent_print_banner(p, &sess);
    struct disp disp = {.trail = 1};
    struct spinner *spinner = spinner_new("working...");
    struct md_renderer *md = markdown_enabled() ? md_new(md_emit_to_disp, &disp) : NULL;
    struct input *input = input_new();
    input_history_open_default(input);
    struct transcript_view tv = {
        /* Point at session fields so the Ctrl-T callback always sees
         * the latest values — the items vector grows via xrealloc so
         * its address changes, and these are read-only views. The cast
         * appeases C's lack of implicit multi-level const conversion. */
        .sys_ref = (const char *const *)&sess.sys,
        .items_ref = &sess.items,
        .n_items_ref = &sess.n_items,
    };
    input_set_transcript_cb(input, show_transcript_cb, &tv);
    /* Initialize once — captures the canonical termios baseline and starts
     * the watcher thread. Idempotent; safe even when stdin/stdout aren't
     * ttys (becomes a no-op in that case). */
    interrupt_init();

    const char *prompt = locale_have_utf8() ? PROMPT_UTF8 : PROMPT_ASCII;

    /* Cluster state lives across the inner loop so consecutive quiet
     * tool calls (read/grep/find...) collapse into a tight block
     * without intervening blank lines. Reset implicitly at the top of
     * each user turn — cluster_terminate is a no-op when inactive, so
     * leftover state from a prior turn (impossible in practice, but
     * inexpensive to be defensive about) is harmless. */
    struct cluster cl = {0};

    for (;;) {
        cluster_terminate(&cl, &disp, spinner);
        disp_block_separator(&disp);
        char *line = input_readline(input, prompt);
        if (!line) {
            putchar('\n');
            break;
        }
        if (!*line) {
            free(line);
            continue;
        }

        /* Slash commands run locally (clear history, show help, ...) and
         * never reach the model. Caught before history-add so recognized
         * /-prefixed lines don't pollute up-arrow recall. Lines that
         * look like commands but aren't (e.g. "/tmp/foo" — the
         * dispatcher's bareword check) return SLASH_NOT_A_COMMAND and
         * fall through to the regular model path below. */
        struct slash_ctx sctx = {.sess = &sess, .provider = p};
        if (slash_dispatch(line, &sctx) != SLASH_NOT_A_COMMAND) {
            free(line);
            disp.trail = 1;
            continue;
        }

        input_history_add(input, line);

        /* Mark the turn boundary just before the user message, not just
         * before the model request that consumes it. The transcript
         * renderer treats a TURN_BOUNDARY as the start-of-turn rule;
         * placing it here puts the user's input under its own turn
         * header (turn 1 = user types + first model response). The
         * inner loop's subsequent iterations insert their own
         * boundaries for follow-up round-trips after tool dispatch. */
        agent_session_add_user(&sess, line);
        free(line);
        /* input_readline left the cursor at column 0 of a fresh row. */
        disp.trail = 1;

        /* Aggregated across every model call this user turn produces.
         * ctx and cached track the latest reported value (= current window
         * state, since each call's input subsumes the prior call's prefix);
         * out is a running sum so the summary reflects total tokens
         * generated in response to this prompt. -1 means "no call reported
         * this number yet". */
        long turn_ctx = -1, turn_out = -1, turn_cached = -1;
        int turn_errored = 0;
        int turn_interrupted = 0;

        /* Arm the watcher for the duration of the inner loop — Esc from
         * here on aborts the stream or running tool. Cleared first so a
         * stray Esc from a previous turn (e.g. user typed Esc during
         * readline editing) doesn't auto-cancel this one. */
        interrupt_clear();
        interrupt_arm();
        /* The first iteration consumes the boundary that was placed
         * with the user message above; subsequent iterations (when the
         * model called tools and we loop back for the next round-trip)
         * insert their own. */
        int first_inner = 1;
        for (;;) {
            if (!first_inner) {
                /* Inserted before ctx is built so the items_append's
                 * potential xrealloc can't dangle ctx.items, and so
                 * this call's n_items already reflects it (providers
                 * skip ITEM_TURN_BOUNDARY in their item-translation
                 * switch). */
                agent_session_add_boundary(&sess);
            }
            first_inner = 0;
            struct context ctx = agent_session_context(&sess);

            /* Spinner sits on its own line as a block: separator first so
             * we have a known column-0 row, then show. The thread starts
             * drawing immediately and hides on the first visible event.
             * Label is reset per-stream so a previous turn's "thinking..."
             * doesn't carry over into a model wait that produces no
             * reasoning deltas. */
            /* If the previous tool batch ended on an active quiet
             * cluster, the inline spinner is still up at the end of
             * `[read] foo, bar`. Don't flicker it off — the next event
             * from the provider will terminate the cluster naturally
             * (text → cluster_terminate in on_event; tool call →
             * dispatch routes through silent path which hides+rewrites
             * inline, or verbose path which terminates). The dedicated
             * line-mode spinner only takes over when we're actually
             * sitting at column 0 with no inline glyph. */
            if (!cl.active) {
                disp_block_separator(&disp);
                spinner_set_label(spinner, "working...");
                spinner_show(spinner);
            }

            if (md)
                md_reset(md);
            struct turn t;
            turn_init(&t);
            disp.saw_text = 0;
            struct event_ctx ec = {.disp = &disp,
                                   .turn = &t,
                                   .spinner = spinner,
                                   .md = md,
                                   .cl = &cl,
                                   .usage = {-1, -1, -1}};
            p->stream(p, &ctx, sess.model, on_event, &ec);

            /* Either a tool-only response (no text emitted, spinner still
             * visible) or stream returned without ever firing an event —
             * make sure we're back to a clean line before continuing.
             *
             * Skip when a quiet cluster is active: the inline spinner is
             * the cluster's "still alive" indicator, sitting at the end
             * of an unterminated line that the next silent dispatch will
             * either coalesce into or close cleanly. Hiding here would
             * lose the in-line cursor position and trick the next
             * dispatch's was_inline=0 branch into thinking the spinner
             * had auto-transitioned, suppressing the \n that should
             * separate the next [read] header from the prior line. */
            if (!cl.active)
                spinner_hide(spinner);
            /* Commit any markdown tail/styles so we don't carry them
             * forward and the terminal isn't left styled. */
            if (md)
                md_flush(md);

            if (t.error) {
                turn_reset(&t);
                turn_errored = 1;
                break;
            }

            /* Settle before deciding what to do with the response — Esc
             * pressed in the last ~50ms of the stream may still be in
             * the classifier's CSI/SS3-vs-bare window, and we must not
             * dispatch tools (or send another request) on a flag that's
             * about to flip. */
            interrupt_settle();
            int interrupted = interrupt_requested();

            if (ec.usage.input_tokens >= 0 && ec.usage.output_tokens >= 0)
                turn_ctx = ec.usage.input_tokens + ec.usage.output_tokens;
            if (ec.usage.output_tokens >= 0)
                turn_out = (turn_out < 0 ? 0 : turn_out) + ec.usage.output_tokens;
            if (ec.usage.cached_tokens >= 0)
                turn_cached = ec.usage.cached_tokens;

            /* On interrupt, finalize any in-flight assistant text with a
             * tag so the next turn carries the marker. `t.in_text` is
             * captured before flush since turn_flush_text clears it. */
            int had_partial_text = interrupted && t.in_text;
            if (interrupted)
                turn_flush_text(&t, had_partial_text ? "\n" INTERRUPT_MARKER : NULL);

            size_t n_before;
            int had_tool_call;
            agent_session_absorb(&sess, &t, &n_before, &had_tool_call);
            turn_reset(&t);

            if (interrupted) {
                /* Synthesize "[interrupted]" results for any completed
                 * tool_calls in this batch (none have run yet). Pending/
                 * incomplete tool_calls were already discarded by
                 * turn_reset above. */
                int marker_placed = had_partial_text;
                size_t end = sess.n_items;
                for (size_t i = n_before; i < end; i++) {
                    if (sess.items[i].kind == ITEM_TOOL_CALL) {
                        items_append(&sess.items, &sess.n_items, &sess.cap_items,
                                     (struct item){
                                         .kind = ITEM_TOOL_RESULT,
                                         .call_id = xstrdup(sess.items[i].call_id),
                                         .output = xstrdup(INTERRUPT_MARKER),
                                     });
                        marker_placed = 1;
                    }
                }
                /* Nothing to tag in the partial output — synthesize a
                 * standalone assistant message so the model sees a
                 * marker on the next turn. Covers the "Esc before any
                 * deltas arrived" case and the "only normally-flushed
                 * text completed" case. */
                if (!marker_placed) {
                    items_append(&sess.items, &sess.n_items, &sess.cap_items,
                                 (struct item){
                                     .kind = ITEM_ASSISTANT_MESSAGE,
                                     .text = xstrdup(INTERRUPT_MARKER),
                                 });
                }
                turn_interrupted = 1;
                break;
            }

            if (!had_tool_call)
                break;

            /* Execute tool calls just added — render header + output as
             * one block per call so parallel calls don't interleave. The
             * spinner runs on the line between header and output so a
             * slow tool still gives the user a "still working" signal;
             * spinner_hide erases that line and tool output writes there
             * in its place. Esc partway through the batch flips remaining
             * calls to a synthesized "[interrupted]" result so the
             * conversation stays well-formed; settle first so a
             * fast-returning tool doesn't race past a pending \x1b in
             * the classifier. */
            size_t current_end = sess.n_items;
            for (size_t i = n_before; i < current_end; i++) {
                if (sess.items[i].kind != ITEM_TOOL_CALL)
                    continue;
                interrupt_settle();
                struct item result;
                if (sess.n_tools == 0) {
                    /* --raw advertised no tools; refuse to run anything
                     * the provider returned anyway. Same gate exists in
                     * oneshot.c — local execution must not be reachable
                     * from a malformed or malicious backend response. */
                    cluster_terminate(&cl, &disp, spinner);
                    result = dispatch_tool_refused(&disp, &sess.items[i]);
                } else if (interrupt_requested()) {
                    cluster_terminate(&cl, &disp, spinner);
                    result = dispatch_tool_skipped(&disp, &sess.items[i]);
                } else {
                    result = dispatch_tool_call(&cl, &disp, spinner, &sess.items[i]);
                }
                items_append(&sess.items, &sess.n_items, &sess.cap_items, result);
            }

            /* Esc fired during or just after this batch. Stop the inner
             * loop without another model call, and ensure history carries
             * a marker — bash appends its own "[interrupted]" footer when
             * killed, but read/write/edit return clean results that
             * would otherwise hide the abort from the next turn. Settle
             * first so we don't race past a pending \x1b. */
            interrupt_settle();
            if (interrupt_requested()) {
                if (sess.n_items == 0 || !tool_result_is_marked(&sess.items[sess.n_items - 1])) {
                    items_append(&sess.items, &sess.n_items, &sess.cap_items,
                                 (struct item){
                                     .kind = ITEM_ASSISTANT_MESSAGE,
                                     .text = xstrdup(INTERRUPT_MARKER),
                                 });
                }
                turn_interrupted = 1;
                break;
            }
        }
        interrupt_disarm();

        if (turn_interrupted) {
            cluster_terminate(&cl, &disp, spinner);
            disp_block_separator(&disp);
            disp_raw(ANSI_DIM);
            disp_printf(&disp, "%s", INTERRUPT_MARKER);
            disp_raw(ANSI_RESET);
            disp_putc(&disp, '\n');
            fflush(stdout);
        }

        if (!turn_errored)
            display_usage(&disp, &cl, spinner, turn_ctx, turn_out, turn_cached);
    }

    spinner_free(spinner);
    input_free(input);
    if (md)
        md_free(md);
    agent_session_free(&sess);
    return 0;
}
