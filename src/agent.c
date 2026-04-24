/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <editline/readline.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
#include "turn.h"
#include "util.h"

#define PROMPT "> "

#define DEFAULT_MODEL "gpt-5.3-codex"
#define DEFAULT_SYSTEM_PROMPT                                                                      \
    "You are hax, a minimalist coding assistant running in the user's terminal. "                  \
    "You have access to `read` and `bash` tools. Prefer action over explanation: "                 \
    "when a question can be answered by running a command or reading a file, do so. "              \
    "Be concise in your replies."

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

static void display_tool_end(struct pending_tool *p)
{
    const struct tool *t = find_tool(p->name);
    const char *display = NULL;
    json_t *root = NULL;
    if (t && t->def.display_arg && p->args.data) {
        json_error_t jerr;
        root = json_loads(p->args.data, 0, &jerr);
        if (root)
            display = json_string_value(json_object_get(root, t->def.display_arg));
    }
    if (display)
        fprintf(stdout, "\x1b[1m%s\x1b[0m\n", display);
    else if (p->args.data && p->args.len)
        fprintf(stdout, "\x1b[2m%s\x1b[0m\n", p->args.data);
    else
        fputc('\n', stdout);
    fflush(stdout);
    if (root)
        json_decref(root);
}

static int on_event(const struct stream_event *ev, void *user)
{
    struct turn *t = user;

    /* Display first so EV_TOOL_CALL_END can inspect pending args before
     * turn_on_event consumes them. For other events the order is immaterial. */
    switch (ev->kind) {
    case EV_TEXT_DELTA:
        fputs(ev->u.text_delta.text, stdout);
        fflush(stdout);
        break;
    case EV_TOOL_CALL_START:
        fprintf(stdout, "\n\x1b[36m[%s]\x1b[0m ", ev->u.tool_call_start.name);
        fflush(stdout);
        break;
    case EV_TOOL_CALL_DELTA:
        break;
    case EV_TOOL_CALL_END: {
        struct pending_tool *p = turn_find_pending(t, ev->u.tool_call_end.id);
        if (p)
            display_tool_end(p);
        break;
    }
    case EV_DONE:
        fputc('\n', stdout);
        fflush(stdout);
        break;
    case EV_ERROR:
        fprintf(stderr, "\n\x1b[31m[error: %s]\x1b[0m\n", ev->u.error.message);
        break;
    }

    turn_on_event(ev, t);
    return 0;
}

static void print_tool_output_preview(const char *out)
{
    size_t n = strlen(out);
    size_t show = n < 400 ? n : 400;
    fputs("\x1b[2m", stdout);
    fwrite(out, 1, show, stdout);
    if (n > show)
        fputs("...", stdout);
    fputs("\x1b[0m\n", stdout);
    fflush(stdout);
}

int agent_run(struct provider *p)
{
    const char *model = getenv("HAX_MODEL");
    if (!model || !*model)
        model = DEFAULT_MODEL;
    const char *sys = getenv("HAX_SYSTEM_PROMPT");
    if (!sys || !*sys)
        sys = DEFAULT_SYSTEM_PROMPT;

    struct tool_def *tools = xmalloc(N_TOOLS * sizeof(*tools));
    for (size_t i = 0; i < N_TOOLS; i++)
        tools[i] = TOOLS[i]->def;

    struct item *items = NULL;
    size_t n_items = 0, cap_items = 0;

    printf("hax (model: %s). Ctrl-D to quit.\n", model);

    for (;;) {
        putchar('\n');
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
        putchar('\n');

        for (;;) {
            struct context ctx = {
                .system_prompt = sys,
                .items = items,
                .n_items = n_items,
                .tools = tools,
                .n_tools = N_TOOLS,
            };

            struct turn t;
            turn_init(&t);
            p->stream(p, &ctx, model, on_event, &t);

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

            /* Execute tool calls that were just added. */
            size_t current_end = n_items;
            for (size_t i = n_before; i < current_end; i++) {
                if (items[i].kind != ITEM_TOOL_CALL)
                    continue;
                char *out = dispatch_tool(items[i].tool_name, items[i].tool_arguments_json);
                print_tool_output_preview(out);
                items_append(&items, &n_items, &cap_items,
                             (struct item){
                                 .kind = ITEM_TOOL_RESULT,
                                 .call_id = xstrdup(items[i].call_id),
                                 .output = out,
                             });
            }
        }
    }

    for (size_t i = 0; i < n_items; i++)
        item_free(&items[i]);
    free(items);
    free(tools);
    return 0;
}
