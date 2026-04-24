/* SPDX-License-Identifier: MIT */
#include "agent.h"

#include <editline/readline.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool.h"
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

struct pending_tool {
    char *call_id;
    char *name;
    struct buf args;
};

struct acc {
    struct item *items;
    size_t n_items;
    size_t cap_items;

    int in_text;
    struct buf text_buf;

    struct pending_tool *pending;
    size_t n_pending;
    size_t cap_pending;

    int error;
};

static struct pending_tool *acc_find_pending(struct acc *a, const char *call_id)
{
    if (!call_id)
        return NULL;
    for (size_t i = 0; i < a->n_pending; i++) {
        if (a->pending[i].call_id && strcmp(a->pending[i].call_id, call_id) == 0)
            return &a->pending[i];
    }
    return NULL;
}

static void acc_flush_text(struct acc *a)
{
    if (!a->in_text)
        return;
    struct item it = {
        .kind = ITEM_ASSISTANT_MESSAGE,
        .text = buf_steal(&a->text_buf),
    };
    items_append(&a->items, &a->n_items, &a->cap_items, it);
    a->in_text = 0;
}

static int on_event(const struct stream_event *ev, void *user)
{
    struct acc *a = user;
    switch (ev->kind) {
    case EV_TEXT_DELTA: {
        const char *t = ev->u.text_delta.text;
        fputs(t, stdout);
        fflush(stdout);
        buf_append_str(&a->text_buf, t);
        a->in_text = 1;
        break;
    }
    case EV_TOOL_CALL_START: {
        acc_flush_text(a);
        fprintf(stdout, "\n\x1b[36m[%s]\x1b[0m ", ev->u.tool_call_start.name);
        fflush(stdout);
        if (a->n_pending == a->cap_pending) {
            size_t c = a->cap_pending ? a->cap_pending * 2 : 4;
            a->pending = xrealloc(a->pending, c * sizeof(*a->pending));
            a->cap_pending = c;
        }
        struct pending_tool *p = &a->pending[a->n_pending++];
        p->call_id = xstrdup(ev->u.tool_call_start.id);
        p->name = xstrdup(ev->u.tool_call_start.name);
        buf_init(&p->args);
        break;
    }
    case EV_TOOL_CALL_DELTA: {
        struct pending_tool *p = acc_find_pending(a, ev->u.tool_call_delta.id);
        if (!p)
            break;
        buf_append_str(&p->args, ev->u.tool_call_delta.args_delta);
        break;
    }
    case EV_TOOL_CALL_END: {
        struct pending_tool *p = acc_find_pending(a, ev->u.tool_call_end.id);
        if (!p)
            break;

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

        struct item it = {
            .kind = ITEM_TOOL_CALL,
            .call_id = xstrdup(p->call_id),
            .tool_name = xstrdup(p->name),
            .tool_arguments_json = buf_steal(&p->args),
        };
        items_append(&a->items, &a->n_items, &a->cap_items, it);
        break;
    }
    case EV_DONE:
        acc_flush_text(a);
        fputc('\n', stdout);
        fflush(stdout);
        break;
    case EV_ERROR:
        acc_flush_text(a);
        fprintf(stderr, "\n\x1b[31m[error: %s]\x1b[0m\n", ev->u.error.message);
        a->error = 1;
        break;
    }
    return 0;
}

static void acc_reset(struct acc *a)
{
    for (size_t i = 0; i < a->n_pending; i++) {
        free(a->pending[i].call_id);
        free(a->pending[i].name);
        buf_free(&a->pending[i].args);
    }
    free(a->pending);
    buf_free(&a->text_buf);
    /* On the error path, items may still own their strings — the success
     * path nulls a->items after transferring ownership to the main vector,
     * so this loop is a no-op there. */
    for (size_t i = 0; i < a->n_items; i++)
        item_free(&a->items[i]);
    free(a->items);
    memset(a, 0, sizeof(*a));
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

            struct acc a;
            memset(&a, 0, sizeof(a));
            p->stream(p, &ctx, model, on_event, &a);

            if (a.error) {
                acc_reset(&a);
                break;
            }

            size_t n_before = n_items;
            int had_tool_call = 0;
            for (size_t i = 0; i < a.n_items; i++) {
                if (a.items[i].kind == ITEM_TOOL_CALL)
                    had_tool_call = 1;
                items_append(&items, &n_items, &cap_items, a.items[i]);
            }
            free(a.items);
            a.items = NULL;
            a.n_items = a.cap_items = 0;
            acc_reset(&a);

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
