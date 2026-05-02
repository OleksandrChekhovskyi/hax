/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <jansson.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ansi.h"
#include "ctrl_strip.h"
#include "env.h"
#include "input.h"
#include "interrupt.h"
#include "markdown.h"
#include "spinner.h"
#include "tool.h"
#include "turn.h"
#include "util.h"

#define INTERRUPT_MARKER "[interrupted]"

/* The ASCII fallback is used when locale_init_utf8() couldn't establish a
 * UTF-8 LC_CTYPE — wcwidth() under a non-UTF-8 locale would mis-account
 * the multibyte glyph and break cursor positioning. */
#define PROMPT_UTF8  ANSI_MAGENTA ANSI_BOLD "❯" ANSI_BOLD_OFF ANSI_FG_DEFAULT " "
#define PROMPT_ASCII ANSI_BOLD ">" ANSI_BOLD_OFF " "

#define DEFAULT_SYSTEM_PROMPT                                                                      \
    "You are hax, a minimalist coding assistant running in the user's terminal. "                  \
    "You have access to `read`, `bash`, `write`, and `edit` tools.\n"                              \
    "\n"                                                                                           \
    "Prefer action over explanation: when a question can be answered by running a "                \
    "command or reading a file, do so. Be concise: no preambles, no trailing "                     \
    "summaries, no filler. Reference code as path:line.\n"                                         \
    "\n"                                                                                           \
    "Before starting a substantial piece of work, say one sentence about what "                    \
    "you're about to do. Don't narrate every read or routine step.\n"                              \
    "\n"                                                                                           \
    "Project guidance in any AGENTS.md block below overrides these defaults.\n"                    \
    "\n"                                                                                           \
    "When changing code:\n"                                                                        \
    "- Make the smallest correct change that fits the existing style.\n"                           \
    "- Fix root causes, not symptoms. Don't fix unrelated bugs unless asked.\n"                    \
    "- Don't introduce new abstractions, helpers, or compatibility shims unless "                  \
    "the task genuinely needs them.\n"                                                             \
    "- Add a comment only when the *why* is non-obvious.\n"                                        \
    "- If the project has a build, tests, or linter, run them before reporting done.\n"            \
    "\n"                                                                                           \
    "Git: never commit, push, amend, branch, or run destructive commands "                         \
    "(`reset --hard`, `checkout --`, `branch -D`) unless the user explicitly asks. "               \
    "Never revert changes you didn't make. If a hook or check fails, fix the cause; "              \
    "don't bypass with `--no-verify`.\n"                                                           \
    "\n"                                                                                           \
    "If asked for a \"review\": lead with bugs, risks, and missing tests for the "                 \
    "*proposed change*, not a summary. A finding should be one the author would "                  \
    "fix if they knew. Skip pre-existing issues and trivial style. Calibrate "                     \
    "severity honestly; no flattery. Empty findings is a valid result."

/* Head-only preview (file-content tools — read top-down). */
#define DISP_HEAD_ONLY_LINES 8
#define DISP_HEAD_ONLY_BYTES 3000

/* Head + tail preview (command-output tools — errors land at the
 * bottom). Whichever side hits its line or byte cap first stops. */
#define DISP_HT_HEAD_LINES 4
#define DISP_HT_TAIL_LINES 4
#define DISP_HT_HEAD_BYTES 1500
#define DISP_HT_TAIL_BYTES 1500

static const struct tool *const TOOLS[] = {
    &TOOL_READ,
    &TOOL_BASH,
    &TOOL_WRITE,
    &TOOL_EDIT,
};
#define N_TOOLS (sizeof(TOOLS) / sizeof(TOOLS[0]))

static const struct tool *find_tool(const char *name)
{
    for (size_t i = 0; i < N_TOOLS; i++) {
        if (strcmp(TOOLS[i]->def.name, name) == 0)
            return TOOLS[i];
    }
    return NULL;
}

/* Tool output flows through ctrl_strip_dup before reaching the display or
 * the conversation history. Tools that wrap arbitrary commands (bash) can
 * surface terminal control bytes — colors, cursor moves, OSC titles, FF
 * clear-screen, charset-shift codes — that would otherwise corrupt the
 * dim-styled preview block and waste tokens once mirrored back to the
 * model. Stripping at this single boundary keeps display and history in
 * sync. */
static char *dispatch_tool(const char *name, const char *args_json)
{
    const struct tool *t = find_tool(name);
    char *raw = t ? t->run(args_json) : xasprintf("unknown tool: %s", name);
    char *clean = ctrl_strip_dup(raw);
    free(raw);
    return clean;
}

static void items_append(struct item **items, size_t *n, size_t *cap, struct item it)
{
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 16;
        *items = xrealloc(*items, c * sizeof(struct item));
        *cap = c;
    }
    (*items)[(*n)++] = it;
}

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

/* ---------- display ----------
 *
 * All terminal output for the conversation flows through these helpers so we
 * can keep visual blocks (user prompt, model text, tool calls) cleanly
 * separated by exactly one blank line, regardless of how many trailing or
 * leading newlines the model and tools happen to emit. */

struct disp {
    int trail;    /* trailing newlines committed to terminal */
    int held;     /* trailing newlines received but not yet committed */
    int saw_text; /* have we emitted real model text yet this turn? */
};

/* Bundles the per-turn state needed by the streaming callback into one
 * struct, so we can plumb display state alongside `struct turn` through
 * the provider's user-pointer parameter. */
struct event_ctx {
    struct disp *disp;
    struct turn *turn;
    struct spinner *spinner;
    struct md_renderer *md; /* NULL when markdown is disabled */
    /* Filled in from EV_DONE; -1/-1/-1 if the provider didn't report. */
    struct stream_usage usage;
};

/* Newline runs at the trail of any disp_* call are deferred into `held`
 * rather than written immediately, so a later disp_block_separator can cap
 * them at 2 (one blank line) — without buffering they'd already be in the
 * terminal and we couldn't take them back. Within a block they get
 * committed verbatim by the next non-NL write. */
static void disp_emit_held(struct disp *d)
{
    for (int i = 0; i < d->held; i++)
        fputc('\n', stdout);
    d->trail += d->held;
    d->held = 0;
}

static void disp_putc(struct disp *d, char c)
{
    if (c == '\n') {
        d->held++;
    } else {
        disp_emit_held(d);
        fputc(c, stdout);
        d->trail = 0;
    }
}

static void disp_write(struct disp *d, const char *s, size_t n)
{
    if (n == 0)
        return;
    /* Walk back across trailing line-ending bytes — both \n and \r — so
     * a CRLF tail (common in Windows files / tool output) is fully
     * deferred and block_separator can collapse it. Only \n counts as a
     * line break for held; \r alone is just a column-zero return. */
    size_t tail_bytes = 0;
    int tail_breaks = 0;
    while (tail_bytes < n) {
        char c = s[n - 1 - tail_bytes];
        if (c == '\n')
            tail_breaks++;
        else if (c != '\r')
            break;
        tail_bytes++;
    }
    if (n > tail_bytes) {
        disp_emit_held(d);
        fwrite(s, 1, n - tail_bytes, stdout);
        d->trail = 0;
    }
    d->held += tail_breaks;
}

/* For ANSI escape sequences (caller guarantees no NLs in s). Doesn't flush
 * held NLs — they remain queued so a later block separator can still cap
 * them. The escape lands ahead of any pending NLs in byte order, but since
 * escapes are zero-width that's visually identical. */
static void disp_raw(const char *s)
{
    fputs(s, stdout);
}

__attribute__((format(printf, 2, 3))) static void disp_printf(struct disp *d, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    disp_write(d, buf, (size_t)n);
    free(buf);
}

/* Bring the terminal trail to exactly one blank line (two NLs). Held NLs
 * from the previous block are dropped, so trailing blank lines in tool
 * output or model text don't leak through. */
static void disp_block_separator(struct disp *d)
{
    int need = 2 - d->trail;
    for (int i = 0; i < need; i++)
        fputc('\n', stdout);
    if (need > 0)
        d->trail += need;
    d->held = 0;
}

/* Strip leading newlines from the first delta of a turn — some compat
 * backends (Qwen on oMLX, in particular) prefix the stream with stray
 * newlines that would push the response visually away from the prompt.
 * Spaces and tabs are preserved: leading indentation can be legitimate
 * response content (code blocks, diff context, etc.). Pure — no stdout
 * writes — so the caller can peek before deciding to hide the spinner. */
static void disp_first_delta_strip(const struct disp *d, const char **s, size_t *n)
{
    if (d->saw_text)
        return;
    while (*n > 0 && (**s == '\n' || **s == '\r')) {
        (*s)++;
        (*n)--;
    }
}

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
        disp_write(d, display_arg, strlen(display_arg));
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
        disp_write(d, call->tool_arguments_json, strlen(call->tool_arguments_json));
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

/* Render a unified diff with ANSI colors — green for added lines, red for
 * removed, cyan for headers and hunk markers, dim for the rare "\ No newline"
 * footer. Diffs are useful enough to the user that we never truncate them
 * (matches Claude Code / pi-mono behavior); the model receives the same
 * text uncolored as the tool result. */
static void display_tool_diff(struct disp *d, const char *diff)
{
    if (!diff || !*diff) {
        disp_raw(ANSI_DIM);
        disp_printf(d, "(no changes)");
        disp_raw(ANSI_RESET);
        disp_putc(d, '\n');
        fflush(stdout);
        return;
    }

    const char *p = diff;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *open = NULL;
        if (len >= 4 && (memcmp(p, "--- ", 4) == 0 || memcmp(p, "+++ ", 4) == 0))
            open = ANSI_DIM; /* file headers — scaffolding, not signal */
        else if (len >= 2 && memcmp(p, "@@", 2) == 0)
            open = ANSI_DIM; /* hunk header */
        else if (len >= 1 && p[0] == '+')
            open = ANSI_GREEN; /* addition */
        else if (len >= 1 && p[0] == '-')
            open = ANSI_RED; /* removal */
        else if (len >= 1 && p[0] == '\\')
            open = ANSI_DIM; /* \ No newline at end of file */
        if (open)
            disp_raw(open);
        disp_write(d, p, len);
        if (open)
            disp_raw(ANSI_RESET);
        disp_putc(d, '\n');
        if (!eol)
            break;
        p = eol + 1;
    }
    fflush(stdout);
}

/* Head-only preview: at most HEAD_ONLY_LINES or HEAD_ONLY_BYTES,
 * whichever is hit first. Truncated previews end with a dim "... (N
 * more lines, M more bytes)" footer. Used by tools whose output is read
 * top-down (file content). */
static void display_tool_output_head(struct disp *d, const char *out)
{
    size_t total = strlen(out);

    size_t cut = 0;
    int lines = 0;
    while (cut < total && lines < DISP_HEAD_ONLY_LINES && cut < DISP_HEAD_ONLY_BYTES) {
        if (out[cut] == '\n')
            lines++;
        cut++;
    }
    int truncated = cut < total;

    disp_raw(ANSI_DIM);
    disp_write(d, out, cut);
    if (truncated) {
        if (d->held == 0 && d->trail == 0)
            disp_putc(d, '\n');
        size_t rem_bytes = total - cut;
        int rem_lines = 0;
        for (size_t i = cut; i < total; i++)
            if (out[i] == '\n')
                rem_lines++;
        if (out[total - 1] != '\n')
            rem_lines++;
        disp_printf(d, "... (%d more line%s, %zu more byte%s)", rem_lines,
                    rem_lines == 1 ? "" : "s", rem_bytes, rem_bytes == 1 ? "" : "s");
        disp_putc(d, '\n');
    } else if (d->held == 0 && d->trail == 0) {
        disp_putc(d, '\n');
    }
    disp_raw(ANSI_RESET);
    fflush(stdout);
}

/* Head + tail preview, with an elision marker between when bytes are
 * dropped. Each side is bounded in lines and bytes; if head and tail
 * meet (short output), the whole thing is shown verbatim. Used by
 * command-output tools (bash) where errors land at the bottom. */
static void display_tool_output_head_tail(struct disp *d, const char *out)
{
    size_t total = strlen(out);

    size_t head_end = 0;
    int head_lines = 0;
    while (head_end < total && head_lines < DISP_HT_HEAD_LINES && head_end < DISP_HT_HEAD_BYTES) {
        if (out[head_end] == '\n')
            head_lines++;
        head_end++;
    }
    if (head_end >= total) {
        disp_raw(ANSI_DIM);
        disp_write(d, out, total);
        if (d->held == 0 && d->trail == 0)
            disp_putc(d, '\n');
        disp_raw(ANSI_RESET);
        fflush(stdout);
        return;
    }

    /* Backward-walk: cross TAIL_LINES-1 newlines, then stop at the next
     * (boundary) newline. tail_start ends up right after the boundary.
     * The byte cap is a safety valve for very long single lines; we
     * accept a partial leading line rather than emit an empty tail. */
    size_t tail_start = total;
    if (tail_start > head_end && out[tail_start - 1] == '\n')
        tail_start--;
    int crossed = 0;
    while (tail_start > head_end && (total - tail_start) < (size_t)DISP_HT_TAIL_BYTES) {
        if (out[tail_start - 1] == '\n') {
            if (crossed == DISP_HT_TAIL_LINES - 1)
                break;
            crossed++;
        }
        tail_start--;
    }

    if (tail_start <= head_end) {
        disp_raw(ANSI_DIM);
        disp_write(d, out, total);
        if (d->held == 0 && d->trail == 0)
            disp_putc(d, '\n');
        disp_raw(ANSI_RESET);
        fflush(stdout);
        return;
    }

    size_t mid_bytes = tail_start - head_end;
    int mid_lines = 0;
    for (size_t i = head_end; i < tail_start; i++)
        if (out[i] == '\n')
            mid_lines++;

    disp_raw(ANSI_DIM);
    disp_write(d, out, head_end);
    if (head_end > 0 && out[head_end - 1] != '\n')
        disp_putc(d, '\n');
    disp_printf(d, "... (%d more line%s, %zu more byte%s) ...", mid_lines,
                mid_lines == 1 ? "" : "s", mid_bytes, mid_bytes == 1 ? "" : "s");
    disp_putc(d, '\n');
    disp_write(d, out + tail_start, total - tail_start);
    if (d->held == 0 && d->trail == 0)
        disp_putc(d, '\n');
    disp_raw(ANSI_RESET);
    fflush(stdout);
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
static void display_usage(struct disp *d, long ctx, long out, long cached)
{
    int show_ctx = ctx >= 0;
    int show_out = out >= 0;
    int show_cached = cached > 0;
    if (!show_ctx && !show_out && !show_cached)
        return;

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
        spinner_hide(ec->spinner);
        if (!d->saw_text) {
            disp_block_separator(d);
            d->saw_text = 1;
        }
        if (ec->md)
            md_feed(ec->md, s, n);
        else
            disp_write(d, s, n);
        fflush(stdout);
        break;
    }
    case EV_TOOL_CALL_START:
    case EV_TOOL_CALL_DELTA:
    case EV_TOOL_CALL_END:
    case EV_REASONING_ITEM:
        /* Suppress live tool-call display — a tool call's header and its
         * output are rendered together as one block during execution so
         * parallel calls don't visually interleave. Reasoning items are
         * also invisible: they're only round-tripped to the next turn. */
        break;
    case EV_DONE:
        ec->usage = ev->u.done.usage;
        fflush(stdout);
        break;
    case EV_ERROR:
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

int agent_run(struct provider *p)
{
    const char *model = getenv("HAX_MODEL");
    if (!model || !*model)
        model = p->default_model;
    if (!model || !*model) {
        fprintf(stderr, "hax: HAX_MODEL is required for provider '%s' (no default)\n",
                p->name ? p->name : "?");
        return 1;
    }
    /* Distinguish unset (use default) from explicit empty ("" — opt out).
     * Some OpenAI-compatible chat templates reject a system message, so
     * users need a way to disable it entirely. */
    const char *sys = getenv("HAX_SYSTEM_PROMPT");
    if (!sys)
        sys = DEFAULT_SYSTEM_PROMPT;
    /* Built once at session start and appended to `sys`; stable across
     * turns so the assembled system prompt stays byte-identical and
     * cache-friendly. The explicit-empty opt-out above suppresses the
     * suffix too — HAX_SYSTEM_PROMPT="" means "send no system message
     * at all", and that includes harness-injected metadata. */
    char *sys_owned = NULL;
    if (*sys) {
        char *suffix = env_build_suffix(model);
        if (suffix) {
            sys_owned = xasprintf("%s\n\n%s", sys, suffix);
            free(suffix);
            sys = sys_owned;
        }
    }
    const char *reasoning_env = getenv("HAX_REASONING_EFFORT");
    const char *reasoning_effort = NULL;
    if (reasoning_env)
        reasoning_effort = *reasoning_env ? reasoning_env : NULL;
    else
        reasoning_effort = p->default_reasoning_effort;

    struct tool_def *tools = xmalloc(N_TOOLS * sizeof(*tools));
    for (size_t i = 0; i < N_TOOLS; i++)
        tools[i] = TOOLS[i]->def;

    struct item *items = NULL;
    size_t n_items = 0, cap_items = 0;

    const char *bar = ANSI_CYAN "▌" ANSI_FG_DEFAULT;
    if (reasoning_effort)
        printf("\n%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s · %s" ANSI_BOLD_OFF
               "\n",
               bar, p->name ? p->name : "?", model, reasoning_effort);
    else
        printf("\n%s " ANSI_BOLD "hax" ANSI_BOLD_OFF " " ANSI_DIM "› %s · %s" ANSI_BOLD_OFF "\n",
               bar, p->name ? p->name : "?", model);
    printf("%s " ANSI_DIM "ctrl-d quit · esc interrupt" ANSI_BOLD_OFF "\n", bar);
    struct disp disp = {.trail = 1};
    struct spinner *spinner = spinner_new("Working...");
    struct md_renderer *md = markdown_enabled() ? md_new(md_emit_to_disp, &disp) : NULL;
    struct input *input = input_new();
    /* Initialize once — captures the canonical termios baseline and starts
     * the watcher thread. Idempotent; safe even when stdin/stdout aren't
     * ttys (becomes a no-op in that case). */
    interrupt_init();

    const char *prompt = locale_have_utf8() ? PROMPT_UTF8 : PROMPT_ASCII;

    for (;;) {
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
        input_history_add(input, line);

        items_append(&items, &n_items, &cap_items,
                     (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup(line)});
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
        for (;;) {
            struct context ctx = {
                .system_prompt = sys,
                .items = items,
                .n_items = n_items,
                .tools = tools,
                .n_tools = N_TOOLS,
                .reasoning_effort = reasoning_effort,
            };

            /* Spinner sits on its own line as a block: separator first so
             * we have a known column-0 row, then show. The thread starts
             * drawing immediately and hides on the first visible event. */
            disp_block_separator(&disp);
            spinner_show(spinner);

            if (md)
                md_reset(md);
            struct turn t;
            turn_init(&t);
            disp.saw_text = 0;
            struct event_ctx ec = {
                .disp = &disp, .turn = &t, .spinner = spinner, .md = md, .usage = {-1, -1, -1}};
            p->stream(p, &ctx, model, on_event, &ec);

            /* Either a tool-only response (no text emitted, spinner still
             * visible) or stream returned without ever firing an event —
             * make sure we're back to a clean line before continuing. */
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

            size_t n_new = 0;
            struct item *new_items = turn_take_items(&t, &n_new);
            turn_reset(&t);

            size_t n_before = n_items;
            int had_tool_call = 0;
            for (size_t i = 0; i < n_new; i++) {
                if (new_items[i].kind == ITEM_TOOL_CALL)
                    had_tool_call = 1;
                items_append(&items, &n_items, &cap_items, new_items[i]);
            }
            free(new_items);

            if (interrupted) {
                /* Synthesize "[interrupted]" results for any completed
                 * tool_calls in this batch (none have run yet). Pending/
                 * incomplete tool_calls were already discarded by
                 * turn_reset above. */
                int marker_placed = had_partial_text;
                size_t end = n_items;
                for (size_t i = n_before; i < end; i++) {
                    if (items[i].kind == ITEM_TOOL_CALL) {
                        items_append(&items, &n_items, &cap_items,
                                     (struct item){
                                         .kind = ITEM_TOOL_RESULT,
                                         .call_id = xstrdup(items[i].call_id),
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
                    items_append(&items, &n_items, &cap_items,
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
             * in its place. */
            size_t current_end = n_items;
            for (size_t i = n_before; i < current_end; i++) {
                if (items[i].kind != ITEM_TOOL_CALL)
                    continue;
                /* Esc since the last iteration — skip the rest of the
                 * batch with synthesized "[interrupted]" results so the
                 * conversation stays well-formed. Show the header so the
                 * user can see which calls were skipped. Settle first
                 * so a fast-returning tool (read/write/edit) doesn't
                 * race past a still-pending \x1b in the classifier. */
                interrupt_settle();
                if (interrupt_requested()) {
                    display_tool_header(&disp, &items[i]);
                    disp_raw(ANSI_DIM);
                    disp_printf(&disp, "%s", INTERRUPT_MARKER);
                    disp_raw(ANSI_RESET);
                    disp_putc(&disp, '\n');
                    fflush(stdout);
                    items_append(&items, &n_items, &cap_items,
                                 (struct item){
                                     .kind = ITEM_TOOL_RESULT,
                                     .call_id = xstrdup(items[i].call_id),
                                     .output = xstrdup(INTERRUPT_MARKER),
                                 });
                    continue;
                }
                display_tool_header(&disp, &items[i]);
                spinner_show(spinner);
                char *out = dispatch_tool(items[i].tool_name, items[i].tool_arguments_json);
                spinner_hide(spinner);
                /* Diff-producing tools render colored and uncapped — but
                 * only when the output actually is a diff. On failure they
                 * return a plain error message that flows through the
                 * normal dim preview path. The `--- ` prefix is the
                 * unambiguous marker; "" is a successful no-op write. */
                const struct tool *t = find_tool(items[i].tool_name);
                int as_diff =
                    t && t->output_is_diff && (out[0] == '\0' || strncmp(out, "--- ", 4) == 0);
                if (as_diff)
                    display_tool_diff(&disp, out);
                else if (t && t->preview_tail)
                    display_tool_output_head_tail(&disp, out);
                else
                    display_tool_output_head(&disp, out);
                items_append(&items, &n_items, &cap_items,
                             (struct item){
                                 .kind = ITEM_TOOL_RESULT,
                                 .call_id = xstrdup(items[i].call_id),
                                 .output = out,
                             });
            }

            /* Esc fired during or just after this batch. Stop the inner
             * loop without another model call, and ensure history carries
             * a marker — bash appends its own "[interrupted]" footer when
             * killed, but read/write/edit return clean results that
             * would otherwise hide the abort from the next turn. Settle
             * first so we don't race past a pending \x1b. */
            interrupt_settle();
            if (interrupt_requested()) {
                if (n_items == 0 || !tool_result_is_marked(&items[n_items - 1])) {
                    items_append(&items, &n_items, &cap_items,
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
            disp_block_separator(&disp);
            disp_raw(ANSI_DIM);
            disp_printf(&disp, "%s", INTERRUPT_MARKER);
            disp_raw(ANSI_RESET);
            disp_putc(&disp, '\n');
            fflush(stdout);
        }

        if (!turn_errored)
            display_usage(&disp, turn_ctx, turn_out, turn_cached);
    }

    spinner_free(spinner);
    input_free(input);
    if (md)
        md_free(md);
    for (size_t i = 0; i < n_items; i++)
        item_free(&items[i]);
    free(items);
    free(tools);
    free(sys_owned);
    return 0;
}
