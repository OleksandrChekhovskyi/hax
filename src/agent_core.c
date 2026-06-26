/* SPDX-License-Identifier: MIT */
#include "agent_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_env.h"
#include "config.h"
#include "util.h"

/* Default text used when HAX_SYSTEM_PROMPT is unset and --raw was not
 * passed. AGENTS.md / env block / skills are appended via agent_env_build_suffix
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
    const char *e = config_str("reasoning_effort");
    if (e)
        return *e ? e : NULL;
    return p ? p->default_reasoning_effort : NULL;
}

char *build_system_prompt(const char *model, int raw)
{
    if (raw)
        return NULL;

    const char *sys = config_str("system_prompt");
    if (!sys)
        sys = DEFAULT_SYSTEM_PROMPT;
    if (!*sys)
        return NULL;

    char *suffix = agent_env_build_suffix(model);
    if (!suffix)
        return xstrdup(sys);

    char *out = xasprintf("%s\n\n%s", sys, suffix);
    free(suffix);
    return out;
}

int agent_session_init(struct agent_session *s, struct provider *p, const struct hax_opts *opts)
{
    memset(s, 0, sizeof(*s));

    /* model may resolve empty: a provider with no default (openai, ollama,
     * …) and nothing configured yet — or no provider at all (`p == NULL`),
     * when the configured/default one couldn't construct (codex not logged
     * in, …) and the user will pick another with /provider. That's no longer
     * fatal — the interactive REPL starts anyway and prompts the user to pick
     * one with /model or /provider, guarding the stream path until they do.
     * The one-shot path, which can't prompt, checks for an empty model /
     * missing provider itself and fails fast there. */
    const char *model = config_str("model");
    if ((!model || !*model) && p)
        model = p->default_model;
    s->model = model ? xstrdup(model) : NULL;
    s->provider_name = p ? p->name : NULL;

    /* --raw collapses to "no system message + no tools advertised" so the
     * model sees only the user text. HAX_SYSTEM_PROMPT="" remains the
     * narrower opt-out (no system message but tools stay). */
    s->raw = opts->raw;
    s->sys = build_system_prompt(s->model, opts->raw);
    const char *effort = resolve_reasoning_effort(p);
    s->reasoning_effort = effort ? xstrdup(effort) : NULL;

    s->n_tools = opts->raw ? 0 : N_TOOLS;
    if (s->n_tools) {
        s->tools = xmalloc(s->n_tools * sizeof(*s->tools));
        for (size_t i = 0; i < s->n_tools; i++)
            s->tools[i] = TOOLS[i]->def;
    }
    return 0;
}

int agent_session_reconfigure(struct agent_session *s, struct provider *p)
{
    const char *model = config_str("model");
    if (!model || !*model)
        model = p->default_model;
    if (!model || !*model) {
        hax_err("no model available for provider '%s' (set one with /model)",
                p->name ? p->name : "?");
        return -1;
    }
    free(s->model);
    s->model = xstrdup(model);
    s->provider_name = p->name;
    /* Rebuild the system prompt so its env block names the new model.
     * Tools and history are deliberately untouched — a switch keeps the
     * conversation going under the new settings. */
    free(s->sys);
    s->sys = build_system_prompt(s->model, s->raw);
    const char *effort = resolve_reasoning_effort(p);
    free(s->reasoning_effort);
    s->reasoning_effort = effort ? xstrdup(effort) : NULL;
    return 0;
}

void agent_session_free(struct agent_session *s)
{
    for (size_t i = 0; i < s->n_items; i++)
        item_free(&s->items[i]);
    free(s->items);
    free(s->tools);
    free(s->sys);
    free(s->model);
    free(s->reasoning_effort);
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
        /* Stamp reasoning with the provider+model that produced it, so a later
         * /model or /provider switch (or a resumed mixed-model file) can't
         * replay its model-bound blob against the wrong backend. Owned by the
         * item (freed in item_free); the session's borrowed names are valid
         * here — the producing provider is still live.
         *
         * The provider stamp is the display name (s->provider_name = p->name),
         * which is intentionally the provider's identity: HAX_PROVIDER_NAME (and
         * config-defined custom providers) name a backend, so it distinguishes
         * two openai-compatible endpoints that share the "openai-compatible"
         * factory id. Renaming a backend therefore re-identifies it and older
         * CoT stops replaying — acceptable, since this is soft reasoning_text
         * (assistant text + tool history always replay); Codex's model-bound
         * encrypted blob rides provider "codex", which has no display override. */
        if (new_items[i].kind == ITEM_REASONING) {
            new_items[i].provider = s->provider_name ? xstrdup(s->provider_name) : NULL;
            new_items[i].model = s->model ? xstrdup(s->model) : NULL;
        }
        items_append(&s->items, &s->n_items, &s->cap_items, new_items[i]);
    }
    free(new_items);
    if (out_had_tool_call)
        *out_had_tool_call = had_tc;
}
