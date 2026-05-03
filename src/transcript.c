/* SPDX-License-Identifier: MIT */
#include "transcript.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "ansi.h"
#include "provider.h"
#include "util.h"

/* Fixed renderer width. Pager passes through; narrow terminals reflow.
 * Picked so the banner box and turn rules stay legible without taking
 * the whole screen on a typical 80-col view. */
#define TRANSCRIPT_WIDTH 60

/* Dim section rule with a label, drawn between top-level blocks. Uses
 * U+2500 BOX DRAWINGS LIGHT HORIZONTAL, which `less -R` passes through
 * unchanged on a UTF-8 terminal. */
static void section(FILE *out, const char *label)
{
    fprintf(out, ANSI_DIM "── %s ──" ANSI_RESET "\n", label);
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
static void banner(FILE *out)
{
    const char *label = "TRANSCRIPT";
    int label_w = (int)strlen(label);
    int inner = TRANSCRIPT_WIDTH - 2; /* minus the two side glyphs */
    int left = (inner - label_w) / 2;
    int right = inner - label_w - left;

    fputs(ANSI_BOLD ANSI_CYAN "┏", out);
    repeat_glyph(out, "━", inner);
    fputs("┓" ANSI_RESET "\n", out);

    fputs(ANSI_BOLD ANSI_CYAN "┃", out);
    repeat_glyph(out, " ", left);
    fputs(label, out);
    repeat_glyph(out, " ", right);
    fputs("┃" ANSI_RESET "\n", out);

    fputs(ANSI_BOLD ANSI_CYAN "┗", out);
    repeat_glyph(out, "━", inner);
    fputs("┛" ANSI_RESET "\n\n", out);
}

/* Turn separator: bold-cyan single-rule with a `# turn N` label.
 * Lighter than the banner box and heavier than the dim `── section ──`
 * sub-rules, so the hierarchy reads top-down — banner → turn → sub.
 * A "turn" here matches turn.c: one model round-trip. Boundaries come
 * from agent-emitted ITEM_TURN_BOUNDARY markers (one per `p->stream`
 * call), so the count stays in lockstep with the dispatch loop without
 * a parallel heuristic. The `#` prefix is a grep anchor — `/# turn`
 * in less jumps between round-trips. */
static void turn_rule(FILE *out, int n)
{
    char label[32];
    int label_w = snprintf(label, sizeof(label), " # turn %d ", n);
    int rule = TRANSCRIPT_WIDTH - label_w;
    if (rule < 4)
        rule = 4;
    int left = rule / 2;
    int right = rule - left;

    fputs(ANSI_BOLD ANSI_CYAN, out);
    repeat_glyph(out, "─", left);
    fputs(label, out);
    repeat_glyph(out, "─", right);
    fputs(ANSI_RESET "\n\n", out);
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

/* User messages: magenta `▌ ` prefix on every line, body also magenta.
 * Mirrors input.c's render_submitted so user turns in the transcript
 * look identical to how they appeared at submit time. */
static void render_user(FILE *out, const struct item *it)
{
    const char *bar = ANSI_MAGENTA "▌ ";
    const char *p = it->text ? it->text : "";
    fputs(bar, out);
    while (*p) {
        if (*p == '\n') {
            fputs(ANSI_FG_DEFAULT "\n", out);
            fputs(bar, out);
            p++;
        } else {
            fputc(*p++, out);
        }
    }
    fputs(ANSI_FG_DEFAULT "\n", out);
}

static void render_assistant(FILE *out, const struct item *it)
{
    section(out, "assistant");
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
static void render_tool_call(FILE *out, const struct item *it)
{
    fprintf(out, ANSI_CYAN "[%s]" ANSI_RESET "\n", it->tool_name ? it->tool_name : "?");

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

static void render_tool_result(FILE *out, const struct item *it)
{
    section(out, "tool result");
    if (it->output)
        fputs(it->output, out);
    ensure_newline(out, it->output);
}

/* Reasoning items are opaque encrypted blobs (Codex CoT round-trip).
 * Showing the raw payload helps no one — surface a one-line marker with
 * the id when present, so the transcript reflects that the turn carried
 * one without bloating the view. */
static void render_reasoning(FILE *out, const struct item *it)
{
    fputs(ANSI_DIM "[reasoning]" ANSI_RESET, out);
    if (it->reasoning_json) {
        json_t *root = json_loads(it->reasoning_json, 0, NULL);
        if (root) {
            const char *id = json_string_value(json_object_get(root, "id"));
            if (id)
                fprintf(out, " " ANSI_DIM "%s" ANSI_RESET, id);
            json_decref(root);
        }
    }
    fputc('\n', out);
}

void transcript_render(FILE *out, const char *system_prompt, const struct item *items,
                       size_t n_items)
{
    banner(out);

    if (system_prompt && *system_prompt) {
        section(out, "system prompt");
        fputs(system_prompt, out);
        ensure_newline(out, system_prompt);
        fputc('\n', out);
    }

    if (n_items == 0)
        return;

    /* The agent's history vector serializes a parallel-tool batch as
     * (CALL_1, CALL_2, ..., RESULT_1, RESULT_2, ...) — the canonical
     * over-the-wire order. The transcript reader wants each call paired
     * with its result inline, matching the live UI. We achieve this by
     * looking up the matching result (by call_id) when rendering each
     * call, then skipping that result when the iteration reaches it.
     * `result_emitted[i]` flags TOOL_RESULTs that have already been
     * paired and printed. */
    char *result_emitted = xcalloc(n_items, 1);

    int turn_no = 0;
    for (size_t i = 0; i < n_items; i++) {
        const struct item *it = &items[i];
        if (it->kind == ITEM_TOOL_RESULT && result_emitted[i])
            continue;
        switch (it->kind) {
        case ITEM_TURN_BOUNDARY:
            turn_rule(out, ++turn_no);
            /* Skip the trailing newline emitted by other branches —
             * turn_rule already lays out its own spacing. */
            continue;
        case ITEM_USER_MESSAGE:
            render_user(out, it);
            break;
        case ITEM_ASSISTANT_MESSAGE:
            render_assistant(out, it);
            break;
        case ITEM_TOOL_CALL:
            render_tool_call(out, it);
            /* Find the matching result later in the vector and
             * render it inline so the user sees a contiguous
             * call+result block per call, even in a batch. */
            if (it->call_id) {
                for (size_t j = i + 1; j < n_items; j++) {
                    if (items[j].kind == ITEM_TOOL_RESULT && items[j].call_id &&
                        strcmp(items[j].call_id, it->call_id) == 0) {
                        fputc('\n', out);
                        render_tool_result(out, &items[j]);
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
            render_tool_result(out, it);
            break;
        case ITEM_REASONING:
            render_reasoning(out, it);
            break;
        }
        fputc('\n', out);
    }

    free(result_emitted);
}
