/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <editline/readline.h>
#include <jansson.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spinner.h"
#include "tool.h"
#include "turn.h"
#include "util.h"

#define PROMPT "> "

#define DEFAULT_SYSTEM_PROMPT                                                                      \
    "You are hax, a minimalist coding assistant running in the user's terminal. "                  \
    "You have access to `read` and `bash` tools. Prefer action over explanation: "                 \
    "when a question can be answered by running a command or reading a file, do so. "              \
    "Be concise in your replies."

#define TOOL_OUTPUT_MAX_LINES 5
#define TOOL_OUTPUT_MAX_BYTES 2000

static const struct tool *const TOOLS[] = {
    &TOOL_READ,
    &TOOL_BASH,
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

static char *dispatch_tool(const char *name, const char *args_json)
{
    const struct tool *t = find_tool(name);
    if (t)
        return t->run(args_json);
    return xasprintf("unknown tool: %s", name);
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
    disp_raw("\x1b[36m");
    disp_printf(d, "[%s]", call->tool_name);
    disp_raw("\x1b[0m");

    if (display_arg) {
        disp_putc(d, ' ');
        disp_raw("\x1b[1m");
        disp_write(d, display_arg, strlen(display_arg));
        disp_raw("\x1b[0m");
    } else if (call->tool_arguments_json && *call->tool_arguments_json) {
        disp_putc(d, ' ');
        disp_raw("\x1b[2m");
        disp_write(d, call->tool_arguments_json, strlen(call->tool_arguments_json));
        disp_raw("\x1b[0m");
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

/* Show a capped preview of tool output: at most TOOL_OUTPUT_MAX_LINES lines
 * or TOOL_OUTPUT_MAX_BYTES bytes, whichever is hit first. Truncated previews
 * end with a dim "... (N more lines, M more bytes)" footer on its own line. */
static void display_tool_output(struct disp *d, const char *out)
{
    size_t total = strlen(out);

    size_t cut = 0;
    int lines = 0;
    while (cut < total && lines < TOOL_OUTPUT_MAX_LINES && cut < TOOL_OUTPUT_MAX_BYTES) {
        if (out[cut] == '\n')
            lines++;
        cut++;
    }
    int truncated = cut < total;

    disp_raw("\x1b[2m");
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
    disp_raw("\x1b[0m");
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
        disp_write(d, s, n);
        fflush(stdout);
        break;
    }
    case EV_TOOL_CALL_START:
    case EV_TOOL_CALL_DELTA:
    case EV_TOOL_CALL_END:
        /* Suppress live tool-call display — a tool call's header and its
         * output are rendered together as one block during execution so
         * parallel calls don't visually interleave. */
        break;
    case EV_DONE:
        fflush(stdout);
        break;
    case EV_ERROR:
        spinner_hide(ec->spinner);
        disp_block_separator(d);
        disp_raw("\x1b[31m");
        disp_printf(d, "[error: %s]", ev->u.error.message);
        disp_raw("\x1b[0m");
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

    struct tool_def *tools = xmalloc(N_TOOLS * sizeof(*tools));
    for (size_t i = 0; i < N_TOOLS; i++)
        tools[i] = TOOLS[i]->def;

    struct item *items = NULL;
    size_t n_items = 0, cap_items = 0;

    printf("hax (provider: %s, model: %s). Ctrl-D to quit.\n", p->name ? p->name : "?", model);
    struct disp disp = {.trail = 1};
    struct spinner *spinner = spinner_new("Working...");

    for (;;) {
        disp_block_separator(&disp);
        char *line = readline(PROMPT);
        if (!line) {
            putchar('\n');
            break;
        }
        if (!*line) {
            free(line);
            continue;
        }
        add_history(line);

        items_append(&items, &n_items, &cap_items,
                     (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup(line)});
        free(line);
        /* libedit echoed the prompt, the line, and a trailing \n. */
        disp.trail = 1;

        for (;;) {
            struct context ctx = {
                .system_prompt = sys,
                .items = items,
                .n_items = n_items,
                .tools = tools,
                .n_tools = N_TOOLS,
            };

            /* Spinner sits on its own line as a block: separator first so
             * we have a known column-0 row, then show. The thread starts
             * drawing immediately and hides on the first visible event. */
            disp_block_separator(&disp);
            spinner_show(spinner);

            struct turn t;
            turn_init(&t);
            disp.saw_text = 0;
            struct event_ctx ec = {.disp = &disp, .turn = &t, .spinner = spinner};
            p->stream(p, &ctx, model, on_event, &ec);

            /* Either a tool-only response (no text emitted, spinner still
             * visible) or stream returned without ever firing an event —
             * make sure we're back to a clean line before continuing. */
            spinner_hide(spinner);

            if (t.error) {
                turn_reset(&t);
                break;
            }

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
                display_tool_header(&disp, &items[i]);
                spinner_show(spinner);
                char *out = dispatch_tool(items[i].tool_name, items[i].tool_arguments_json);
                spinner_hide(spinner);
                display_tool_output(&disp, out);
                items_append(&items, &n_items, &cap_items,
                             (struct item){
                                 .kind = ITEM_TOOL_RESULT,
                                 .call_id = xstrdup(items[i].call_id),
                                 .output = out,
                             });
            }
        }
    }

    spinner_free(spinner);
    for (size_t i = 0; i < n_items; i++)
        item_free(&items[i]);
    free(items);
    free(tools);
    return 0;
}
