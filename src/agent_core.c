/* SPDX-License-Identifier: MIT */
#include "agent_core.h"

#include <stdlib.h>
#include <string.h>

#include "agent_env.h"
#include "agent_usage.h"
#include "config.h"
#include "effort.h"
#include "model_meta.h"
#include "provider.h"
#include "tool.h"
#include "turn.h"
#include "util.h"
#include "providers/registry.h"
#include "tools/bash_env.h"

/* Dynamic environment and project guidance are appended by build_system_prompt. */
static const char DEFAULT_SYSTEM_PROMPT[] =
    "You are hax, a minimalist coding assistant running in the user's terminal.\n"
    "\n"
    "Prefer action over explanation: when a question can be answered by running a "
    "command or reading a file, do so. Be concise: no filler, no trailing "
    "summaries. Reference code as path:line. Before substantial work, say in one "
    "sentence what you're about to do; while working, mention only meaningful "
    "developments (a root cause, a change of direction, a blocker worth a "
    "decision), not routine steps.\n"
    "\n"
    "When something is ambiguous, infer from the code and pick a sensible default "
    "rather than stopping. Ask only when genuinely blocked: the choice materially "
    "changes the result, an action is destructive or affects shared state, or you "
    "need a value you can't obtain. To ask, end your turn with one targeted "
    "question and a recommended default.\n"
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

static const struct tool *const TOOLS[] = {
    &TOOL_READ, &TOOL_EDIT, &TOOL_WRITE, &TOOL_BASH, &TOOL_TASK_WAIT, &TOOL_TASK_KILL,
};
static const size_t N_TOOLS = sizeof(TOOLS) / sizeof(TOOLS[0]);

const struct tool *agent_find_tool(const char *name)
{
    for (size_t i = 0; i < N_TOOLS; i++) {
        if (strcmp(TOOLS[i]->def.name, name) == 0)
            return TOOLS[i];
    }
    return NULL;
}

void agent_session_append(struct agent_session *session, struct item item)
{
    if (session->n_items == session->cap_items) {
        size_t capacity = session->cap_items ? session->cap_items * 2 : 16;
        session->items = xrealloc(session->items, capacity * sizeof(*session->items));
        session->cap_items = capacity;
    }
    session->items[session->n_items++] = item;
}

static char *resolve_effort(const struct provider *provider, const char *model)
{
    struct effort_set levels;
    model_meta_efforts(provider, model, &levels);
    if (levels.count == 0)
        return NULL;

    /* Provider defaults can also name a level that the selected model rejects. */
    const char *default_effort = provider ? provider->default_effort : NULL;
    if (default_effort && *default_effort && !effort_set_has(&levels, default_effort))
        default_effort = effort_clamp(&levels, default_effort);

    const char *requested = config_str("effort");
    if (!requested)
        return (default_effort && *default_effort) ? xstrdup(default_effort) : NULL;
    if (!*requested)
        return NULL;
    if (effort_set_has(&levels, requested))
        return xstrdup(requested);

    /* A setting carried across models is clamped instead of sent as an invalid level. */
    const char *clamped = effort_clamp(&levels, requested);
    if (clamped)
        return xstrdup(clamped);
    return (default_effort && *default_effort) ? xstrdup(default_effort) : NULL;
}

static char *build_system_prompt(const char *model_label, int raw)
{
    if (raw)
        return NULL;

    const char *base_prompt = config_str("system_prompt");
    if (!base_prompt)
        base_prompt = DEFAULT_SYSTEM_PROMPT;
    if (!*base_prompt)
        return NULL;

    char *suffix = agent_env_build_suffix(model_label);
    if (!suffix)
        return xstrdup(base_prompt);

    char *system_prompt = xasprintf("%s\n\n%s", base_prompt, suffix);
    free(suffix);
    return system_prompt;
}

static char *resolve_model_label(struct provider *provider, const char *model)
{
    if (!model)
        return NULL;
    return (provider && provider->model_label) ? provider->model_label(provider, model)
                                               : xstrdup(model);
}

const char *agent_provider_id(const struct provider *provider)
{
    const char *id = config_str("provider");
    if (id && *id)
        return id;
    return provider ? provider->name : NULL;
}

const char *agent_provider_log_name(const struct provider *provider)
{
    const char *id = agent_provider_id(provider);
    return (id && *id) ? id : "none";
}

int agent_recording_enabled(const struct provider *provider)
{
    const char *id = agent_provider_id(provider);
    const struct provider_factory *factory = id ? provider_find(id) : NULL;
    return !config_bool_or("no_session", factory && factory->internal);
}

/* Keep subprocess inheritance synchronized at every settings-resolution point. */
static void export_selection(const struct provider *provider, const struct agent_session *session)
{
    bash_env_set_selection(agent_provider_id(provider), session->model, session->effort);
}

void agent_session_init(struct agent_session *session, struct provider *provider,
                        const struct hax_opts *opts)
{
    memset(session, 0, sizeof(*session));

    const char *model = config_str("model");
    if ((!model || !*model) && provider)
        model = provider->default_model;
    session->model = model ? xstrdup(model) : NULL;
    session->model_label = resolve_model_label(provider, session->model);
    session->provider_name = provider ? provider->name : NULL;

    /* An empty system prompt suppresses only that message; raw mode also suppresses tools. */
    session->raw_mode = opts->raw;
    session->system_prompt = build_system_prompt(session->model_label, opts->raw);
    session->effort = resolve_effort(provider, session->model);

    if (!opts->raw) {
        session->tools = xmalloc(N_TOOLS * sizeof(*session->tools));
        for (size_t i = 0; i < N_TOOLS; i++) {
            const struct tool_def *def =
                TOOLS[i]->advertise ? TOOLS[i]->advertise() : &TOOLS[i]->def;
            if (def)
                session->tools[session->n_tools++] = *def;
        }
    }
    export_selection(provider, session);
}

int agent_session_reconfigure(struct agent_session *session, struct provider *provider)
{
    const char *model = config_str("model");
    if (!model || !*model)
        model = provider->default_model;
    if (!model || !*model) {
        hax_err("no model available for provider '%s' (set one with /model)",
                provider->name ? provider->name : "?");
        return -1;
    }
    char *new_model = xstrdup(model);
    char *new_model_label = resolve_model_label(provider, new_model);
    free(session->model);
    free(session->model_label);
    session->model = new_model;
    session->model_label = new_model_label;
    session->provider_name = provider->name;
    /* The Environment section embeds the selected model. */
    free(session->system_prompt);
    session->system_prompt = build_system_prompt(session->model_label, session->raw_mode);
    char *effort = resolve_effort(provider, session->model);
    free(session->effort);
    session->effort = effort;
    export_selection(provider, session);
    return 0;
}

int agent_session_resync_effort(struct agent_session *session, struct provider *provider,
                                char **previous)
{
    if (previous)
        *previous = NULL;
    if (!session || !provider || !session->model || !*session->model)
        return 0;
    model_meta_wait(provider);
    char *effort = resolve_effort(provider, session->model);
    int unchanged = (!effort && !session->effort) ||
                    (effort && session->effort && strcmp(effort, session->effort) == 0);
    if (unchanged) {
        free(effort);
        return 0;
    }
    if (previous)
        *previous = session->effort;
    else
        free(session->effort);
    session->effort = effort;
    export_selection(provider, session);
    return 1;
}

void agent_session_free(struct agent_session *session)
{
    for (size_t i = 0; i < session->n_items; i++)
        item_free(&session->items[i]);
    free(session->items);
    free(session->tools);
    free(session->system_prompt);
    free(session->model);
    free(session->model_label);
    free(session->effort);
    memset(session, 0, sizeof(*session));
}

void agent_session_reset(struct agent_session *session)
{
    for (size_t i = 0; i < session->n_items; i++)
        item_free(&session->items[i]);
    session->n_items = 0;
}

struct context agent_session_context(const struct agent_session *session)
{
    /* Derived, not tracked: /undo, /fork, and a resumed file all land on the right answer with no
     * cached index to keep honest. */
    size_t floor = items_context_floor(session->items, session->n_items);
    /* Ctrl-T reaches an untouched session, and offsetting a null pointer is undefined even by
     * zero. */
    struct item *items = session->items ? session->items + floor : NULL;
    return (struct context){
        .system_prompt = session->system_prompt,
        .items = items,
        .n_items = session->n_items - floor,
        .tools = session->tools,
        .n_tools = session->n_tools,
        .effort = session->effort,
        /* Unknown by default (adapters treat it as yes); callers that
         * know the provider overwrite with model_meta_image_input(). */
        .image_input = -1,
    };
}

void agent_session_add_user(struct agent_session *session, const char *text)
{
    agent_session_append(session, (struct item){.kind = ITEM_TURN_BOUNDARY});
    agent_session_append(session, (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup(text)});
}

void agent_session_add_continuation(struct agent_session *session)
{
    agent_session_append(session, (struct item){.kind = ITEM_TURN_BOUNDARY});
    agent_session_append(session, (struct item){
                                      .kind = ITEM_USER_MESSAGE,
                                      .text = xstrdup(CONTINUE_MARKER),
                                      .origin = ITEM_ORIGIN_CONTINUATION,
                                  });
}

void agent_session_add_boundary(struct agent_session *session)
{
    agent_session_append(session, (struct item){.kind = ITEM_TURN_BOUNDARY});
}

/* An interrupted tool may already carry the marker as its output suffix. */
static int tool_result_has_interrupt_marker(const struct item *it)
{
    if (it->kind != ITEM_TOOL_RESULT || !it->output)
        return 0;
    size_t out_len = strlen(it->output);
    size_t marker_len = strlen(INTERRUPT_MARKER);
    if (out_len < marker_len)
        return 0;
    return strcmp(it->output + out_len - marker_len, INTERRUPT_MARKER) == 0;
}

void agent_session_mark_interrupt(struct agent_session *session)
{
    /* Look past inert trailing items (usage footers, boundaries) to the
     * last *content* item — footer emission between the tool batch and
     * this check must not hide an already-marked result and provoke a
     * duplicate marker. */
    size_t i = session->n_items;
    while (i > 0 && (session->items[i - 1].kind == ITEM_TURN_USAGE ||
                     session->items[i - 1].kind == ITEM_TURN_BOUNDARY))
        i--;
    if (i > 0 && tool_result_has_interrupt_marker(&session->items[i - 1]))
        return;
    agent_session_append(session, (struct item){
                                      .kind = ITEM_ASSISTANT_MESSAGE,
                                      .text = xstrdup(INTERRUPT_MARKER),
                                      .origin = ITEM_ORIGIN_INTERRUPTED,
                                  });
}

void agent_session_add_turn_usage(struct agent_session *session, const struct provider *provider,
                                  const struct stream_usage *usage, long elapsed_ms)
{
    struct turn_usage *turn_usage =
        agent_turn_usage_new(usage, elapsed_ms, provider, session->model);
    if (!turn_usage)
        return;
    agent_session_append(
        session, (struct item){
                     .kind = ITEM_TURN_USAGE,
                     .usage = turn_usage,
                     .provider = session->provider_name ? xstrdup(session->provider_name) : NULL,
                     .model = session->model && *session->model ? xstrdup(session->model) : NULL,
                 });
}

struct agent_absorb_result agent_session_absorb(struct agent_session *session, struct turn *turn)
{
    struct agent_absorb_result result = {.items_from = session->n_items};
    size_t count = 0;
    struct item *items = turn_take_items(turn, &count);

    for (size_t i = 0; i < count; i++) {
        if (items[i].kind == ITEM_TOOL_CALL)
            result.had_tool_call = 1;
        /* Reasoning can be model-bound. Display identity also distinguishes custom endpoints
         * that share one provider factory. */
        if (items[i].kind == ITEM_REASONING) {
            items[i].provider = session->provider_name ? xstrdup(session->provider_name) : NULL;
            items[i].model = session->model ? xstrdup(session->model) : NULL;
        }
        agent_session_append(session, items[i]);
    }
    free(items);
    return result;
}
