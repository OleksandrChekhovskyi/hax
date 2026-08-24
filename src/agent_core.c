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
#include "session.h"
#include "tool.h"
#include "transcript.h"
#include "turn.h"
#include "util.h"
#include "providers/registry.h"
#include "tools/bash_env.h"
#include "tools/task_registry.h"

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
    &TOOL_READ, &TOOL_EDIT, &TOOL_WRITE, &TOOL_BASH, &TOOL_TASK_WAIT,
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

/* Expand a configured prompt value, falling back so the session still comes up when an @file
 * has gone missing. NULL `value` selects `fallback`; NULL `fallback` means skip on failure. */
static char *resolve_prompt(const char *value, const char *fallback)
{
    if (!value)
        value = fallback;
    if (!value)
        return NULL;

    char *error = NULL;
    char *text = config_prompt_expand(value, &error);
    if (!text) {
        hax_warn("%s — %s", error ? error : "couldn't expand prompt value",
                 fallback ? "using the built-in prompt" : "skipping");
        free(error);
        return fallback ? xstrdup(fallback) : NULL;
    }
    return text;
}

static void append_prompt_part(struct buf *b, const char *part)
{
    if (!part || !*part)
        return;
    if (b->len > 0)
        buf_append_str(b, "\n\n");
    buf_append_str(b, part);
}

static char *build_system_prompt(const char *model_label, int raw)
{
    if (raw)
        return NULL;

    /* "(none)" opts out of the system message entirely; "" empties only the base prompt. */
    const char *base_value = config_str("system_prompt");
    if (base_value && strcmp(base_value, "(none)") == 0)
        return NULL;

    char *base = resolve_prompt(base_value, DEFAULT_SYSTEM_PROMPT);
    char *append = resolve_prompt(config_str("system_prompt_append"), NULL);
    char *suffix = agent_env_build_suffix(model_label);

    struct buf b;
    buf_init(&b);
    append_prompt_part(&b, base);
    append_prompt_part(&b, append);
    append_prompt_part(&b, suffix);
    free(base);
    free(append);
    free(suffix);

    if (b.len == 0) {
        buf_free(&b);
        return NULL;
    }
    return buf_steal(&b);
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
        return provider_canonical_id(id);
    return provider ? provider_stable_id(provider) : NULL;
}

const char *agent_provider_log_name(const struct provider *provider)
{
    const char *id = agent_provider_id(provider);
    return (id && *id) ? id : "none";
}

int agent_recording_enabled(const struct provider *provider)
{
    const char *id = agent_provider_id(provider);
    const struct provider_def *def = id ? provider_find(id) : NULL;
    return !config_bool_or("no_session", def && def->internal);
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
    session->provider_id = provider ? provider_stable_id(provider) : NULL;

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
    session->provider_id = provider_stable_id(provider);
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
    /* Bounded: this runs on the interactive foreground thread, and a router-autoload probe can
     * take minutes. A late report refines effort before the next prompt instead. */
    model_meta_wait_ms(provider, MODEL_META_PROBE_WAIT_MS);
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

/* An ordinary session then stores nothing extra, while a renamed provider or a gguf path still
 * reads as the banner showed it. */
static char *label_if_distinct(const char *label, const char *wire_id)
{
    if (!label || !*label || (wire_id && strcmp(label, wire_id) == 0))
        return NULL;
    return xstrdup(label);
}

static void fill_provenance(struct turn_provenance *provenance, struct agent_session *session,
                            const struct provider *provider, const struct stream_response *response)
{
    provenance->provider_label =
        label_if_distinct(provider ? provider->name : NULL, session->provider_id);
    provenance->model_label = label_if_distinct(session->model_label, session->model);
    provenance->effort = session->effort && *session->effort ? xstrdup(session->effort) : NULL;
    if (!response)
        return;

    provenance->served_model = label_if_distinct(response->model, session->model);
    provenance->route = response->route ? xstrdup(response->route) : NULL;
    provenance->response_id = response->id ? xstrdup(response->id) : NULL;
}

void agent_session_add_turn_usage(struct agent_session *session, const struct provider *provider,
                                  const struct stream_usage *usage, long elapsed_ms,
                                  const struct stream_response *response)
{
    struct turn_usage *turn_usage =
        agent_turn_usage_new(usage, elapsed_ms, provider, session->model);
    if (!turn_usage)
        return;
    fill_provenance(&turn_usage->provenance, session, provider, response);
    agent_session_append(
        session, (struct item){
                     .kind = ITEM_TURN_USAGE,
                     .usage = turn_usage,
                     .provider = session->provider_id ? xstrdup(session->provider_id) : NULL,
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
         * that share one constructor. */
        if (items[i].kind == ITEM_REASONING) {
            items[i].provider = session->provider_id ? xstrdup(session->provider_id) : NULL;
            items[i].model = session->model ? xstrdup(session->model) : NULL;
        }
        agent_session_append(session, items[i]);
    }
    free(items);
    return result;
}

void agent_flush_logs(struct transcript_log *tlog, struct session_log *slog,
                      const struct item *items, size_t n_items)
{
    transcript_log_append(tlog, items, n_items);
    session_log_append(slog, items, n_items);
}

void agent_finalize_tasks(struct agent_session *session, struct transcript_log *tlog,
                          struct session_log *slog)
{
    /* Record the terminal state before the shutdown destroys uncollected output. */
    char *exit_note = task_exit_note();
    if (exit_note)
        agent_session_append(session, (struct item){
                                          .kind = ITEM_USER_MESSAGE,
                                          .text = exit_note,
                                          .origin = ITEM_ORIGIN_TASK_NOTE,
                                      });
    /* The note is bookkeeping, not a turn: it must not commit a staged selection (such as a
     * `/new <preset>` meant for the next conversation) into the record being left. */
    session_log_discard_selection(slog);
    agent_flush_logs(tlog, slog, session->items, session->n_items);
    task_registry_shutdown();
}
