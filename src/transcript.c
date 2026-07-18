/* SPDX-License-Identifier: MIT */
#include "transcript.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "provider.h"
#include "util.h"
#include "render/diff_color.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Fixed renderer width. Pager passes through; narrow terminals reflow.
 * Picked so the banner box and turn rules stay legible without taking
 * the whole screen on a typical 80-col view. */
#define TRANSCRIPT_WIDTH 60

/* Threaded as `int color` through every helper: when true, ANSI escapes
 * are emitted as designed (Ctrl-T → less -R); when false, the output is
 * pure plain text suitable for `cat`/`grep`/diff (HAX_TRANSCRIPT log,
 * primarily consumed by other agents). Each call site uses ANSI_IF(seq)
 * so the conditional collapses to "" at compile-time-ish cost when
 * color is false. */
#define ANSI_IF(seq) (color ? (seq) : "")

/* Dim section rule with a label, drawn between top-level blocks. Uses
 * U+2500 BOX DRAWINGS LIGHT HORIZONTAL, which `less -R` passes through
 * unchanged on a UTF-8 terminal. */
static void section(FILE *out, int color, const char *label)
{
    fprintf(out, "%s── %s ──%s\n", ANSI_IF(ANSI_DIM), label, ANSI_IF(ANSI_RESET));
}

/* Repeat a UTF-8 glyph (any byte length) `n` times. */
static void repeat_glyph(FILE *out, const char *glyph, int n)
{
    for (int i = 0; i < n; i++)
        fputs(glyph, out);
}

/* Bold chrome-colored three-line box, drawn at the top of every
 * transcript page.
 * Visual weight is the point — it has to be unambiguous through the
 * pager's `q`-then-glance. The banner lives at the very top so there's
 * no need to make it grep-friendly; only turn rules carry the `#`
 * anchor that helps `/# turn` jump between round-trips.
 *
 * Each row opens with bold + the chrome color and closes with ANSI_RESET
 * before the newline: `less -R` resets SGR state at every line break,
 * so leaving the box "open" across rows would render rows 2 and 3 in
 * default fg. */
static void banner(FILE *out, int color)
{
    const char *label = "TRANSCRIPT";
    int label_w = (int)strlen(label);
    int inner = TRANSCRIPT_WIDTH - 2; /* minus the two side glyphs */
    int left = (inner - label_w) / 2;
    int right = inner - label_w - left;

    fputs(ANSI_IF(ANSI_BOLD), out);
    fputs(ANSI_IF(theme_open(THEME_CHROME)), out);
    fputs("┏", out);
    repeat_glyph(out, "━", inner);
    fputs("┓", out);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputc('\n', out);

    fputs(ANSI_IF(ANSI_BOLD), out);
    fputs(ANSI_IF(theme_open(THEME_CHROME)), out);
    fputs("┃", out);
    repeat_glyph(out, " ", left);
    fputs(label, out);
    repeat_glyph(out, " ", right);
    fputs("┃", out);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputc('\n', out);

    fputs(ANSI_IF(ANSI_BOLD), out);
    fputs(ANSI_IF(theme_open(THEME_CHROME)), out);
    fputs("┗", out);
    repeat_glyph(out, "━", inner);
    fputs("┛", out);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputs("\n\n", out);
}

/* Turn separator: bold chrome-colored single-rule with a `# turn N` label.
 * Lighter than the banner box and heavier than the dim `── section ──`
 * sub-rules, so the hierarchy reads top-down — banner → turn → sub.
 * A "turn" here matches turn.c: one model round-trip. Boundaries come
 * from agent-emitted ITEM_TURN_BOUNDARY markers (one per `p->stream`
 * call), so the count stays in lockstep with the dispatch loop without
 * a parallel heuristic. The `#` prefix is a grep anchor — `/# turn`
 * in less jumps between round-trips. */
static void turn_rule(FILE *out, int color, int n)
{
    char label[32];
    int label_w = snprintf(label, sizeof(label), " # turn %d ", n);
    int rule = TRANSCRIPT_WIDTH - label_w;
    if (rule < 4)
        rule = 4;
    int left = rule / 2;
    int right = rule - left;

    fputs(ANSI_IF(ANSI_BOLD), out);
    fputs(ANSI_IF(theme_open(THEME_CHROME)), out);
    repeat_glyph(out, "─", left);
    fputs(label, out);
    repeat_glyph(out, "─", right);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputs("\n\n", out);
}

/* Append a trailing newline if `s` doesn't already end with one. Avoids
 * double-blank-lines while still guaranteeing the next block starts on
 * column 0. */
static void ensure_newline(FILE *out, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || s[n - 1] != '\n')
        fputc('\n', out);
}

/* Body text with `open`/`close` ANSI sequences re-applied per line:
 * `less -R` resets SGR state at every line break, so a single leading
 * escape would drop after the first newline (and a soft-wrapped long
 * line keeps its color either way). Shared by every colored-body block
 * — user (accent), reasoning and the compaction seed (dim). */
static void render_body_lines(FILE *out, int color, const char *text, const char *open,
                              const char *close)
{
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (color)
            fputs(open, out);
        fwrite(p, 1, n, out);
        if (color)
            fputs(close, out);
        if (!nl)
            break;
        fputc('\n', out);
        p = nl + 1;
    }
    ensure_newline(out, text);
}

/* User messages: section() rule with the "user" label, then the body
 * in the accent color. The transcript is intentionally raw — no wrap,
 * no markdown, no per-line prefix — so the section rule plays the
 * same role for user messages that the `── assistant ──` / `── tool
 * result ──` rules play for the model side. A per-line `▌ ` strip
 * would break visually whenever a long line gets soft-wrapped by the
 * pager, so the rule is the only anchor; color carries the rest.
 *
 * A compaction seed goes on the wire as a user message, but rendering
 * it accent-colored would read as "the user typed this whole summary".
 * The transcript's visual grammar keeps the accent exclusively for
 * typed input, so the seed gets its own label and the dim body
 * machinery shares with reasoning — full text, just visibly not human. */
static void render_user(FILE *out, int color, const struct item *it)
{
    const char *text = it->text ? it->text : "";
    if (it->compact_seed) {
        section(out, color, "compaction seed");
        render_body_lines(out, color, text, ANSI_DIM, ANSI_RESET);
        return;
    }
    section(out, color, "user");
    render_body_lines(out, color, text, theme_open(THEME_ACCENT), theme_close(THEME_ACCENT));
}

static void render_assistant(FILE *out, int color, const struct item *it)
{
    section(out, color, "assistant");
    if (it->text)
        fputs(it->text, out);
    ensure_newline(out, it->text);
}

/* Tool call: `[name]` in chrome, then args. Pretty-prints when the args parse
 * as JSON; otherwise dumps verbatim so a malformed payload still appears
 * in the transcript instead of silently disappearing. The call_id is
 * intentionally omitted — pairing happens inline (the matching result
 * is rendered directly below), so the id would be visual noise. It
 * remains available via HAX_TRACE for protocol-level debugging. */
static void render_tool_call(FILE *out, int color, const struct item *it)
{
    fprintf(out, "%s[%s]%s\n", ANSI_IF(theme_open(THEME_CHROME)),
            it->tool_name ? it->tool_name : "?", ANSI_IF(ANSI_RESET));

    if (it->tool_arguments_json && *it->tool_arguments_json) {
        json_t *root = json_loads(it->tool_arguments_json, 0, NULL);
        if (root) {
            char *pretty = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
            if (pretty) {
                fputs(pretty, out);
                ensure_newline(out, pretty);
                free(pretty);
            } else {
                /* json_dumps NULL means encode failed (deeply nested
                 * input or similar) — fall back to verbatim so the
                 * call doesn't go silently empty. */
                fputs(it->tool_arguments_json, out);
                ensure_newline(out, it->tool_arguments_json);
            }
            json_decref(root);
        } else {
            fputs(it->tool_arguments_json, out);
            ensure_newline(out, it->tool_arguments_json);
        }
    }
}

/* Pretty-print a JSON literal, falling back to a verbatim dump when
 * parsing or encoding fails (deeply nested input, malformed payload).
 * Writes a leading newline before the body so the schema sits on its own
 * line below the description. */
static void render_json_indented(FILE *out, const char *json_text)
{
    if (!json_text || !*json_text)
        return;
    fputc('\n', out);
    json_t *root = json_loads(json_text, 0, NULL);
    if (root) {
        char *pretty = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
        if (pretty) {
            fputs(pretty, out);
            ensure_newline(out, pretty);
            free(pretty);
        } else {
            fputs(json_text, out);
            ensure_newline(out, json_text);
        }
        json_decref(root);
    } else {
        fputs(json_text, out);
        ensure_newline(out, json_text);
    }
}

/* Tools section: one block per advertised tool, mirroring exactly the
 * fields the provider serializes for the model — name, description,
 * parameters_schema_json. The agent's display_arg / preview_tail / etc.
 * are UI-only and intentionally omitted. The chrome `[name]` header
 * matches render_tool_call so a reader scanning the transcript can
 * spot the matching tool definition above any specific call. */
static void render_tools(FILE *out, int color, const struct tool_def *tools, size_t n)
{
    if (!tools || n == 0)
        return;
    section(out, color, "tools");
    for (size_t i = 0; i < n; i++) {
        fprintf(out, "%s[%s]%s\n", ANSI_IF(theme_open(THEME_CHROME)),
                tools[i].name ? tools[i].name : "?", ANSI_IF(ANSI_RESET));
        if (tools[i].description) {
            fputs(tools[i].description, out);
            ensure_newline(out, tools[i].description);
        }
        render_json_indented(out, tools[i].parameters_schema_json);
        if (i + 1 < n)
            fputc('\n', out);
    }
    fputc('\n', out);
}

/* Byte length of read.c's `cat -n` style line prefix — leading spaces,
 * at least one digit, then the READ_LINE_DELIM arrow — or 0 when the
 * line doesn't start with one. Matching the exact producer format keeps
 * false positives to lines that would render identically anyway. */
static size_t read_prefix_len(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && line[i] == ' ')
        i++;
    size_t digits_at = i;
    while (i < len && line[i] >= '0' && line[i] <= '9')
        i++;
    if (i == digits_at)
        return 0;
    size_t dlen = strlen(READ_LINE_DELIM);
    if (i + dlen > len || memcmp(line + i, READ_LINE_DELIM, dlen) != 0)
        return 0;
    return i + dlen;
}

/* Read result body: dim the line-number gutter, leave content on the
 * default foreground. Lines without the prefix — the trailing
 * `[truncated ...]` marker, error messages — pass through plain, so
 * every non-happy path degrades safely per line. Color-mode only; the
 * plain log takes the verbatim path in render_tool_result. */
static void render_read_body(FILE *out, const char *text)
{
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        size_t plen = read_prefix_len(p, n);
        if (plen > 0) {
            fputs(ANSI_DIM, out);
            fwrite(p, 1, plen, out);
            fputs(ANSI_RESET, out);
            fwrite(p + plen, 1, n - plen, out);
        } else {
            fwrite(p, 1, n, out);
        }
        if (!nl)
            break;
        fputc('\n', out);
        p = nl + 1;
    }
    ensure_newline(out, text);
}

/* Unified-diff body (edit/write results): per-line coloring via the
 * classifier shared with the live preview, but mapped to the
 * transcript's grammar. Context lines stay on the default foreground —
 * dim would demote real file content below the baseline every other
 * tool result renders at (the live preview dims context only because
 * its whole preview is dim). Dim marks the metadata lines instead
 * ("---"/"+++" headers, "@@" markers, "\ No newline"), same role as
 * the read gutter. Unlike the live view, the file headers stay and no
 * line is truncated — the transcript shows the result exactly as the
 * model received it. Color re-opens per line for `less -R`, same as
 * render_body_lines. */
static void render_diff_body(FILE *out, const char *text)
{
    int in_hunk = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        const char *open = NULL;
        switch (diff_line_classify(p, n, in_hunk)) {
        case DIFF_LINE_ADD:
            open = theme_open(THEME_ADD);
            break;
        case DIFF_LINE_REMOVE:
            open = theme_open(THEME_REMOVE);
            break;
        case DIFF_LINE_META:
            open = ANSI_DIM;
            break;
        case DIFF_LINE_CONTEXT:
            break;
        }
        if (open)
            fputs(open, out);
        fwrite(p, 1, n, out);
        if (open)
            fputs(ANSI_RESET, out);
        if (n >= 2 && memcmp(p, "@@", 2) == 0)
            in_hunk = 1;
        if (!nl)
            break;
        fputc('\n', out);
        p = nl + 1;
    }
    ensure_newline(out, text);
}

/* `tool_name` comes from the paired TOOL_CALL (results don't carry one);
 * NULL for orphan results. It gates the styled body renderers for the
 * file tools — content stays byte-exact in every mode, styling is pure
 * SGR wrapping and only in color mode, so the plain HAX_TRANSCRIPT log
 * is untouched. Edit/write results are diff-colored only when the body
 * actually is a diff (the same "--- " sniff the live preview's R_DIFF
 * switch uses), which excludes edit errors and write's "created ..."
 * new-file confirmation. */
static void render_tool_result(FILE *out, int color, const struct item *it, const char *tool_name)
{
    const char *text = it->output ? it->output : "";
    section(out, color, "tool result");
    if (color && tool_name) {
        if (strcmp(tool_name, "read") == 0) {
            render_read_body(out, text);
            return;
        }
        if ((strcmp(tool_name, "edit") == 0 || strcmp(tool_name, "write") == 0) &&
            strncmp(text, "--- ", 4) == 0) {
            render_diff_body(out, text);
            return;
        }
    }
    if (it->output)
        fputs(it->output, out);
    ensure_newline(out, it->output);
}

/* Reasoning items are opaque encrypted blobs (Codex CoT round-trip).
 * Showing the raw payload helps no one — surface a one-line marker with
 * the id when present, so the transcript reflects that the turn carried
 * one without bloating the view. */
static void render_reasoning(FILE *out, int color, const struct item *it)
{
    /* Text CoT (openai-family reasoning_content): a section rule + dim body,
     * matching the user/assistant/tool blocks. */
    if (it->reasoning_text) {
        section(out, color, "reasoning");
        render_body_lines(out, color, it->reasoning_text, ANSI_DIM, ANSI_RESET);
        return;
    }
    /* Codex's opaque structured blob: just the inline tag + id, since there
     * is no human-readable text to show (it's encrypted). */
    fprintf(out, "%s[reasoning]%s", ANSI_IF(ANSI_DIM), ANSI_IF(ANSI_RESET));
    if (it->reasoning_json) {
        json_t *root = json_loads(it->reasoning_json, 0, NULL);
        if (root) {
            const char *id = json_string_value(json_object_get(root, "id"));
            if (id)
                fprintf(out, " %s%s%s", ANSI_IF(ANSI_DIM), id, ANSI_IF(ANSI_RESET));
            json_decref(root);
        }
    }
    fputc('\n', out);
}

static void usage_sep(FILE *out, int *first)
{
    if (!*first)
        fputs(" · ", out);
    *first = 0;
}

/* One "<label> <count> [$cost]" segment; the dollar figure is shown only
 * when it's a positive estimate — a reported exact total is never
 * decomposed (its category costs stay negative), and "$0.00" would be
 * clutter, not information. */
static void usage_tokens(FILE *out, int *first, const char *label, long tokens, double usd)
{
    char t[32];
    usage_sep(out, first);
    format_tokens(t, sizeof(t), tokens);
    fprintf(out, "%s %s", label, t);
    if (usd > 0) {
        char c[32];
        format_cost(c, sizeof(c), usd);
        fprintf(out, " %s", c);
    }
}

/* ITEM_TURN_USAGE: the per-request stats footer — time worked, the
 * request's cost (exact when provider-reported, "~" when estimated from
 * catalog rates), then the token categories with their per-category
 * estimates. One raw line by design: the transcript never wraps content;
 * pagers and editors do. All figures come precomputed in the payload
 * (turn_usage_make), keeping this pure formatting. */
static void render_turn_usage(FILE *out, int color, const struct turn_usage *tu)
{
    const struct stream_usage *u = &tu->usage;
    int first = 1;
    char buf[32];
    fputs(ANSI_IF(ANSI_DIM), out);
    if (tu->elapsed_ms >= 0) {
        format_duration(buf, sizeof(buf), tu->elapsed_ms);
        usage_sep(out, &first);
        fputs(buf, out);
    }
    if (tu->cost_total > 0) {
        format_cost(buf, sizeof(buf), tu->cost_total);
        usage_sep(out, &first);
        fprintf(out, "%s%s", tu->cost_estimated ? "~" : "", buf);
    }
    long cr = u->cached_tokens > 0 ? u->cached_tokens : 0;
    long cw = u->cache_write_tokens > 0 ? u->cache_write_tokens : 0;
    if (u->input_tokens >= 0) {
        /* The categories are non-overlapping: "in" is the uncached
         * remainder, so the counts sum to what was actually sent. */
        long in = u->input_tokens - cr - cw;
        usage_tokens(out, &first, "in", in > 0 ? in : 0, tu->cost_in);
    }
    if (cr > 0)
        usage_tokens(out, &first, "cache", cr, tu->cost_cache_read);
    if (cw > 0)
        usage_tokens(out, &first, "write", cw, tu->cost_cache_write);
    if (u->output_tokens >= 0)
        usage_tokens(out, &first, "out", u->output_tokens, tu->cost_out);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputc('\n', out);
}

void transcript_render_header(FILE *out, int color, const char *system_prompt,
                              const struct tool_def *tools, size_t n_tools)
{
    banner(out, color);

    if (system_prompt && *system_prompt) {
        section(out, color, "system prompt");
        fputs(system_prompt, out);
        ensure_newline(out, system_prompt);
        fputc('\n', out);
    }

    render_tools(out, color, tools, n_tools);
}

void transcript_render_items(FILE *out, int color, const struct item *items, size_t n_items,
                             size_t start_idx, int *turn_no)
{
    if (start_idx >= n_items)
        return;

    /* The agent's history vector serializes a parallel-tool batch as
     * (CALL_1, CALL_2, ..., RESULT_1, RESULT_2, ...) — the canonical
     * over-the-wire order. The transcript reader wants each call paired
     * with its result inline, matching the live UI. We achieve this by
     * looking up the matching result (by call_id) when rendering each
     * call, then skipping that result when the iteration reaches it.
     * `result_emitted[i]` flags TOOL_RESULTs that have already been
     * paired and printed. Sized to n_items so we can index by absolute
     * position even when start_idx > 0; the [0..start_idx) prefix is
     * unused. */
    char *result_emitted = xcalloc(n_items, 1);

    for (size_t i = start_idx; i < n_items; i++) {
        const struct item *it = &items[i];
        if (it->kind == ITEM_TOOL_RESULT && result_emitted[i])
            continue;
        switch (it->kind) {
        case ITEM_TURN_BOUNDARY:
            turn_rule(out, color, ++(*turn_no));
            /* Skip the trailing newline emitted by other branches —
             * turn_rule already lays out its own spacing. */
            continue;
        case ITEM_USER_MESSAGE:
            render_user(out, color, it);
            break;
        case ITEM_ASSISTANT_MESSAGE:
            render_assistant(out, color, it);
            break;
        case ITEM_TOOL_CALL:
            render_tool_call(out, color, it);
            /* Find the matching result later in the vector and
             * render it inline so the user sees a contiguous
             * call+result block per call, even in a batch. */
            if (it->call_id) {
                for (size_t j = i + 1; j < n_items; j++) {
                    if (items[j].kind == ITEM_TOOL_RESULT && items[j].call_id &&
                        strcmp(items[j].call_id, it->call_id) == 0) {
                        fputc('\n', out);
                        render_tool_result(out, color, &items[j], it->tool_name);
                        result_emitted[j] = 1;
                        break;
                    }
                }
            }
            break;
        case ITEM_TOOL_RESULT:
            /* Orphan result (no preceding call with matching id) —
             * shouldn't happen in practice, but rendering it
             * standalone is safer than dropping it. No paired call
             * means no tool name, so the body renders plain. */
            render_tool_result(out, color, it, NULL);
            break;
        case ITEM_REASONING:
            render_reasoning(out, color, it);
            break;
        case ITEM_TURN_USAGE:
            if (!it->usage)
                continue; /* degenerate record — nothing to show */
            render_turn_usage(out, color, it->usage);
            break;
        }
        fputc('\n', out);
    }

    free(result_emitted);
}

void transcript_render(FILE *out, const char *system_prompt, const struct tool_def *tools,
                       size_t n_tools, const struct item *items, size_t n_items)
{
    transcript_render_header(out, 1, system_prompt, tools, n_tools);
    int turn_no = 0;
    transcript_render_items(out, 1, items, n_items, 0, &turn_no);
}

/* ---------------- HAX_TRANSCRIPT log ---------------- */

void transcript_log_init(void)
{
    const char *path = config_str("transcript");
    if (!path || !*path)
        return;
    /* fopen("we") is itself the truncate. Discard the handle — the real
     * open happens later in transcript_log_open, which writes the
     * banner+sys+tools header once those values are known. */
    FILE *fp = fopen(path, "we");
    if (fp)
        fclose(fp);
}

struct transcript_log {
    FILE *fp;
    char *path; /* owned: a borrowed config_str("transcript") would dangle when
                 * a /provider, /model, or /effort commit replaces a config
                 * tier object (and reset reuses path in freopen). */
    size_t n_written;
    int turn_no;
};

struct transcript_log *transcript_log_open(const char *system_prompt, const struct tool_def *tools,
                                           size_t n_tools)
{
    const char *path = config_str("transcript");
    if (!path || !*path)
        return NULL;
    FILE *fp = fopen(path, "we");
    if (!fp) {
        hax_warn("HAX_TRANSCRIPT: cannot open '%s' for writing", path);
        return NULL;
    }
    /* Line buffering so a reader watching the file (editor reload,
     * `tail -f` for a human) sees each block as it lands. Every render
     * path here ends its writes with a newline, so this is effectively
     * per-block flushing without explicit fflush calls — same approach
     * as src/trace.c. */
    setvbuf(fp, NULL, _IOLBF, 0);
    struct transcript_log *log = xmalloc(sizeof(*log));
    log->fp = fp;
    log->path = xstrdup(path);
    log->n_written = 0;
    log->turn_no = 0;
    transcript_render_header(fp, 0, system_prompt, tools, n_tools);
    return log;
}

void transcript_log_append(struct transcript_log *log, const struct item *items, size_t n_items)
{
    /* fp is normally non-NULL once _open returned a log handle, but
     * _reset clears it on freopen failure — guard so an append after a
     * dead reset is a silent no-op rather than a NULL-FILE* crash. */
    if (!log || !log->fp || n_items <= log->n_written)
        return;
    transcript_render_items(log->fp, 0, items, n_items, log->n_written, &log->turn_no);
    log->n_written = n_items;
}

void transcript_log_reset(struct transcript_log *log, const char *system_prompt,
                          const struct tool_def *tools, size_t n_tools)
{
    if (!log)
        return;
    /* Steady state: freopen with "we" truncates the existing path while
     * reusing the same FILE* and fd. Recovery state: a previous reset
     * already failed and closed our fd (log->fp == NULL); freopen on a
     * NULL stream is UB, so fall back to fopen to try re-establishing
     * — the underlying issue (missing directory, full disk) may have
     * been resolved between this /new and the last. Line buffering
     * survives freopen on glibc but not portably — re-set either way. */
    FILE *fp = log->fp ? freopen(log->path, "we", log->fp) : fopen(log->path, "we");
    if (!fp) {
        hax_warn("HAX_TRANSCRIPT: cannot truncate '%s' on /new", log->path);
        log->fp = NULL;
        return;
    }
    log->fp = fp;
    setvbuf(fp, NULL, _IOLBF, 0);
    log->n_written = 0;
    log->turn_no = 0;
    transcript_render_header(fp, 0, system_prompt, tools, n_tools);
}

void transcript_log_close(struct transcript_log *log)
{
    if (!log)
        return;
    if (log->fp)
        fclose(log->fp);
    free(log->path);
    free(log);
}
