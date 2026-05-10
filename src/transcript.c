/* SPDX-License-Identifier: MIT */
#include "transcript.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "util.h"
#include "terminal/ansi.h"

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

/* Bold-cyan three-line box, drawn at the top of every transcript page.
 * Visual weight is the point — it has to be unambiguous through the
 * pager's `q`-then-glance. The banner lives at the very top so there's
 * no need to make it grep-friendly; only turn rules carry the `#`
 * anchor that helps `/# turn` jump between round-trips.
 *
 * Each row opens with ANSI_BOLD ANSI_CYAN and closes with ANSI_RESET
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

    fputs(ANSI_IF(ANSI_BOLD ANSI_CYAN), out);
    fputs("┏", out);
    repeat_glyph(out, "━", inner);
    fputs("┓", out);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputc('\n', out);

    fputs(ANSI_IF(ANSI_BOLD ANSI_CYAN), out);
    fputs("┃", out);
    repeat_glyph(out, " ", left);
    fputs(label, out);
    repeat_glyph(out, " ", right);
    fputs("┃", out);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputc('\n', out);

    fputs(ANSI_IF(ANSI_BOLD ANSI_CYAN), out);
    fputs("┗", out);
    repeat_glyph(out, "━", inner);
    fputs("┛", out);
    fputs(ANSI_IF(ANSI_RESET), out);
    fputs("\n\n", out);
}

/* Turn separator: bold-cyan single-rule with a `# turn N` label.
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

    fputs(ANSI_IF(ANSI_BOLD ANSI_CYAN), out);
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

/* User messages: bright magenta `▌ ` prefix on every line, body also
 * bright magenta. Mirrors input.c's render_submitted so user turns in
 * the transcript look identical to how they appeared at submit time.
 * With color=0 the bar collapses to a plain `▌ ` prefix — still useful
 * as a visual anchor in `cat` output. */
static void render_user(FILE *out, int color, const struct item *it)
{
    const char *open = ANSI_IF(ANSI_BRIGHT_MAGENTA);
    const char *close = ANSI_IF(ANSI_FG_DEFAULT);
    const char *p = it->text ? it->text : "";
    fputs(open, out);
    fputs("▌ ", out);
    while (*p) {
        if (*p == '\n') {
            fputs(close, out);
            fputc('\n', out);
            fputs(open, out);
            fputs("▌ ", out);
            p++;
        } else {
            fputc(*p++, out);
        }
    }
    fputs(close, out);
    fputc('\n', out);
}

static void render_assistant(FILE *out, int color, const struct item *it)
{
    section(out, color, "assistant");
    if (it->text)
        fputs(it->text, out);
    ensure_newline(out, it->text);
}

/* Tool call: `[name]` cyan, then args. Pretty-prints when the args parse
 * as JSON; otherwise dumps verbatim so a malformed payload still appears
 * in the transcript instead of silently disappearing. The call_id is
 * intentionally omitted — pairing happens inline (the matching result
 * is rendered directly below), so the id would be visual noise. It
 * remains available via HAX_TRACE for protocol-level debugging. */
static void render_tool_call(FILE *out, int color, const struct item *it)
{
    fprintf(out, "%s[%s]%s\n", ANSI_IF(ANSI_CYAN), it->tool_name ? it->tool_name : "?",
            ANSI_IF(ANSI_RESET));

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
 * are UI-only and intentionally omitted. The cyan `[name]` header
 * matches render_tool_call so a reader scanning the transcript can
 * spot the matching tool definition above any specific call. */
static void render_tools(FILE *out, int color, const struct tool_def *tools, size_t n)
{
    if (!tools || n == 0)
        return;
    section(out, color, "tools");
    for (size_t i = 0; i < n; i++) {
        fprintf(out, "%s[%s]%s\n", ANSI_IF(ANSI_CYAN), tools[i].name ? tools[i].name : "?",
                ANSI_IF(ANSI_RESET));
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

static void render_tool_result(FILE *out, int color, const struct item *it)
{
    section(out, color, "tool result");
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
                        render_tool_result(out, color, &items[j]);
                        result_emitted[j] = 1;
                        break;
                    }
                }
            }
            break;
        case ITEM_TOOL_RESULT:
            /* Orphan result (no preceding call with matching id) —
             * shouldn't happen in practice, but rendering it
             * standalone is safer than dropping it. */
            render_tool_result(out, color, it);
            break;
        case ITEM_REASONING:
            render_reasoning(out, color, it);
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
    const char *path = getenv("HAX_TRANSCRIPT");
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
    const char *path; /* getenv pointer — stable for the process lifetime */
    size_t n_written;
    int turn_no;
};

struct transcript_log *transcript_log_open(const char *system_prompt, const struct tool_def *tools,
                                           size_t n_tools)
{
    const char *path = getenv("HAX_TRANSCRIPT");
    if (!path || !*path)
        return NULL;
    FILE *fp = fopen(path, "we");
    if (!fp) {
        fprintf(stderr, "hax: HAX_TRANSCRIPT: cannot open '%s' for writing\n", path);
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
    log->path = path;
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
        fprintf(stderr, "hax: HAX_TRANSCRIPT: cannot truncate '%s' on /new\n", log->path);
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
    free(log);
}
