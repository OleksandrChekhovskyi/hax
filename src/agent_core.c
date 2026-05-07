/* SPDX-License-Identifier: MIT */
#include "agent_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "util.h"

/* Default text used when HAX_SYSTEM_PROMPT is unset and --raw was not
 * passed. AGENTS.md / env block / skills are appended via env_build_suffix
 * and the assembled string is what's sent to the provider. */
static const char DEFAULT_SYSTEM_PROMPT[] =
    "You are hax, a minimalist coding assistant running in the user's terminal. "
    "You have access to `read`, `bash`, `write`, and `edit` tools.\n"
    "\n"
    "Prefer action over explanation: when a question can be answered by running a "
    "command or reading a file, do so. Be concise: no preambles, no trailing "
    "summaries, no filler. Reference code as path:line.\n"
    "\n"
    "Before starting a substantial piece of work, say one sentence about what "
    "you're about to do. Don't narrate every read or routine step.\n"
    "\n"
    "Keep working until the task is resolved — don't stop at analysis or a partial "
    "fix, and don't hand back after a recoverable tool error. Stop when the work is "
    "done, you genuinely can't proceed, or you need information only the user can "
    "provide.\n"
    "\n"
    "Project guidance in any AGENTS.md block below overrides these defaults.\n"
    "\n"
    "When changing code:\n"
    "- Make the smallest correct change that fits the existing style.\n"
    "- Fix root causes, not symptoms. Don't fix unrelated bugs unless asked.\n"
    "- Don't introduce new abstractions, helpers, or compatibility shims unless "
    "the task genuinely needs them.\n"
    "- Add a comment only when the *why* is non-obvious.\n"
    "- If the project has a build, tests, or linter, run them before reporting done.\n"
    "\n"
    "Git: never commit, push, amend, branch, or run destructive commands "
    "(`reset --hard`, `checkout --`, `branch -D`) unless the user explicitly asks. "
    "Never revert changes you didn't make. If a hook or check fails, fix the cause; "
    "don't bypass with `--no-verify`.\n"
    "\n"
    "If asked for a \"review\": lead with bugs, risks, and missing tests for the "
    "*proposed change*, not a summary. A finding should be one the author would "
    "fix if they knew. Skip pre-existing issues and trivial style. Calibrate "
    "severity honestly; no flattery. Empty findings is a valid result.";

/* Tool table shared between the interactive REPL and the non-interactive
 * one-shot path: both register the same set, and `--raw` omits the whole
 * list with one decision in agent_session_init. */
static const struct tool *const TOOLS[] = {
    &TOOL_READ,
    &TOOL_BASH,
    &TOOL_WRITE,
    &TOOL_EDIT,
};
static const size_t N_TOOLS = sizeof(TOOLS) / sizeof(TOOLS[0]);

const struct tool *find_tool(const char *name)
{
    for (size_t i = 0; i < N_TOOLS; i++) {
        if (strcmp(TOOLS[i]->def.name, name) == 0)
            return TOOLS[i];
    }
    return NULL;
}

void items_append(struct item **items, size_t *n, size_t *cap, struct item it)
{
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 16;
        *items = xrealloc(*items, c * sizeof(struct item));
        *cap = c;
    }
    (*items)[(*n)++] = it;
}

const char *resolve_reasoning_effort(const struct provider *p)
{
    const char *e = getenv("HAX_REASONING_EFFORT");
    if (e)
        return *e ? e : NULL;
    return p->default_reasoning_effort;
}

char *build_system_prompt(const char *model, int raw)
{
    if (raw)
        return NULL;

    const char *sys = getenv("HAX_SYSTEM_PROMPT");
    if (!sys)
        sys = DEFAULT_SYSTEM_PROMPT;
    if (!*sys)
        return NULL;

    char *suffix = env_build_suffix(model);
    if (!suffix)
        return xstrdup(sys);

    char *out = xasprintf("%s\n\n%s", sys, suffix);
    free(suffix);
    return out;
}

int agent_session_init(struct agent_session *s, struct provider *p, const struct hax_opts *opts)
{
    memset(s, 0, sizeof(*s));

    s->model = getenv("HAX_MODEL");
    if (!s->model || !*s->model)
        s->model = p->default_model;
    if (!s->model || !*s->model) {
        fprintf(stderr, "hax: HAX_MODEL is required for provider '%s' (no default)\n",
                p->name ? p->name : "?");
        return -1;
    }

    /* --raw collapses to "no system message + no tools advertised" so the
     * model sees only the user text. HAX_SYSTEM_PROMPT="" remains the
     * narrower opt-out (no system message but tools stay). */
    s->sys = build_system_prompt(s->model, opts->raw);
    s->reasoning_effort = resolve_reasoning_effort(p);

    s->n_tools = opts->raw ? 0 : N_TOOLS;
    if (s->n_tools) {
        s->tools = xmalloc(s->n_tools * sizeof(*s->tools));
        for (size_t i = 0; i < s->n_tools; i++)
            s->tools[i] = TOOLS[i]->def;
    }
    return 0;
}

void agent_session_free(struct agent_session *s)
{
    for (size_t i = 0; i < s->n_items; i++)
        item_free(&s->items[i]);
    free(s->items);
    free(s->tools);
    free(s->sys);
    memset(s, 0, sizeof(*s));
}

void agent_session_reset(struct agent_session *s)
{
    for (size_t i = 0; i < s->n_items; i++)
        item_free(&s->items[i]);
    s->n_items = 0;
}

struct context agent_session_context(const struct agent_session *s)
{
    return (struct context){
        .system_prompt = s->sys,
        .items = s->items,
        .n_items = s->n_items,
        .tools = s->tools,
        .n_tools = s->n_tools,
        .reasoning_effort = s->reasoning_effort,
    };
}

void agent_session_add_user(struct agent_session *s, const char *text)
{
    items_append(&s->items, &s->n_items, &s->cap_items, (struct item){.kind = ITEM_TURN_BOUNDARY});
    items_append(&s->items, &s->n_items, &s->cap_items,
                 (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup(text)});
}

void agent_session_add_boundary(struct agent_session *s)
{
    items_append(&s->items, &s->n_items, &s->cap_items, (struct item){.kind = ITEM_TURN_BOUNDARY});
}

void agent_session_absorb(struct agent_session *s, struct turn *t, size_t *out_before,
                          int *out_had_tool_call)
{
    size_t n_new = 0;
    struct item *new_items = turn_take_items(t, &n_new);

    if (out_before)
        *out_before = s->n_items;
    int had_tc = 0;
    for (size_t i = 0; i < n_new; i++) {
        if (new_items[i].kind == ITEM_TOOL_CALL)
            had_tc = 1;
        items_append(&s->items, &s->n_items, &s->cap_items, new_items[i]);
    }
    free(new_items);
    if (out_had_tool_call)
        *out_had_tool_call = had_tc;
}
