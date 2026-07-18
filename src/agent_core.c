/* SPDX-License-Identifier: MIT */
#include "agent_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_env.h"
#include "catalog.h"
#include "config.h"
#include "util.h"
#include "tools/bash_export.h"

/* Default text used when HAX_SYSTEM_PROMPT is unset and --raw was not
 * passed. The Environment, AGENTS.md, and skills sections are appended via
 * agent_env_build_suffix; the assembled string is sent to the provider. */
static const char DEFAULT_SYSTEM_PROMPT[] =
    "You are hax, a minimalist coding assistant running in the user's terminal. "
    "You have access to `read`, `edit`, `write`, and `bash` tools.\n"
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

/* Tool table shared between the interactive REPL and the non-interactive
 * one-shot path: both register the same set, and `--raw` omits the whole
 * list with one decision in agent_session_init. */
static const struct tool *const TOOLS[] = {
    &TOOL_READ,
    &TOOL_EDIT,
    &TOOL_WRITE,
    &TOOL_BASH,
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

const char *resolve_effort(const struct provider *p)
{
    /* A persisted/configured effort only makes sense for a provider that
     * exposes a categorical effort ladder. Without one (NULL hook or an empty
     * list — llama.cpp, ollama, a non-reasoning model …) the value can't be
     * sent meaningfully, so don't resolve it: this keeps a stale effort left in
     * state.json from leaking into the banner, the wire request, and the logs
     * after switching to such a provider. */
    const char *const *eff = NULL;
    struct provider *mp = (struct provider *)p;
    size_t n = (p && p->list_efforts) ? p->list_efforts(mp, &eff) : 0;
    if (n == 0)
        return NULL;

    const char *e = config_str("effort");
    if (!e)
        return p->default_effort; /* unset / "(default)" → provider default */
    if (!*e)
        return NULL; /* explicit empty → force omit */

    /* The value must be one the current provider's ladder actually accepts. A
     * stale pick carried in state.json from a different backend (e.g. "medium"
     * persisted under codex, then a switch to a low/high-only provider) would
     * otherwise be sent verbatim and 400 every turn. On a non-member fall back
     * to the provider's default rather than honoring a value it can't take. */
    for (size_t i = 0; i < n; i++)
        if (strcmp(e, eff[i]) == 0)
            return e;
    return p->default_effort;
}

char *build_system_prompt(const char *model_label, int raw)
{
    if (raw)
        return NULL;

    const char *sys = config_str("system_prompt");
    if (!sys)
        sys = DEFAULT_SYSTEM_PROMPT;
    if (!*sys)
        return NULL;

    char *suffix = agent_env_build_suffix(model_label);
    if (!suffix)
        return xstrdup(sys);

    char *out = xasprintf("%s\n\n%s", sys, suffix);
    free(suffix);
    return out;
}

int format_stats_segments(char segs[][STATS_SEG_LEN], long ctx, long limit, long elapsed_ms,
                          double spend, int spend_approx)
{
    int n = 0;
    /* Sized so the longest prefix ("context ", 8 cols) plus the scratch
     * value provably fits in a segment — GCC's -Wformat-truncation flags
     * the snprintfs below otherwise. Humanized values are ~20 chars at
     * worst ("9.9M / 9.9M (100%)"), so the headroom costs nothing. */
    char buf[STATS_SEG_LEN - 16];

    /* Segment order is scope order, narrow to wide: this user turn's
     * activity (worked), then current window state (context), then the
     * session total (spent). */
    if (elapsed_ms >= 0) {
        format_duration(buf, sizeof(buf), elapsed_ms);
        snprintf(segs[n++], STATS_SEG_LEN, "%s", buf);
    }
    if (ctx >= 0) {
        format_context(buf, sizeof(buf), ctx, limit);
        /* The "context" word is disambiguation, not decoration: with an
         * unknown window the figure is a bare number ("8.9k") that
         * nothing identifies. The gauge form ("8.9k / 256k (3%)")
         * self-identifies — next to fields whose units (s, $) label
         * themselves — and drops it. */
        if (limit <= 0)
            snprintf(segs[n++], STATS_SEG_LEN, "context %s", buf);
        else
            snprintf(segs[n++], STATS_SEG_LEN, "%s", buf);
    }
    if (spend > 0) {
        format_cost(buf, sizeof(buf), spend);
        snprintf(segs[n++], STATS_SEG_LEN, "%s%s", spend_approx ? "~" : "", buf);
    }
    return n;
}

void spend_account(struct spend_totals *t, const struct stream_usage *u, const char *catalog_id,
                   const char *model)
{
    if (u->cost >= 0) {
        /* Any non-negative cost is a *reported* charge (stream_usage's
         * convention: negative = not reported) — including an explicit
         * zero, e.g. a free-tier model. Zero must not fall through to the
         * record below, where catalog rates would re-price the free
         * response as paid. */
        t->reported += u->cost;
        return;
    }
    if (u->input_tokens <= 0 && u->output_tokens <= 0)
        return; /* nothing billable was reported */
    /* No reported charge: record the response for catalog estimation at
     * read time (spend_total). */
    if (t->n_recs == t->cap_recs) {
        t->cap_recs = t->cap_recs ? t->cap_recs * 2 : 8;
        t->recs = xrealloc(t->recs, t->cap_recs * sizeof(*t->recs));
    }
    struct spend_rec *r = &t->recs[t->n_recs++];
    r->u = *u;
    r->catalog_id = catalog_id && *catalog_id ? xstrdup(catalog_id) : NULL;
    r->model = model && *model ? xstrdup(model) : NULL;
}

/* Price one record, USD; -1 when it can't be priced (no catalog identity,
 * model unknown to the catalog). `split` (optional) receives the
 * per-category components. */
static double spend_rec_price(const struct spend_rec *r, struct catalog_split *split)
{
    if (!r->catalog_id || !r->model)
        return -1;
    struct catalog_entry e;
    if (catalog_lookup(r->catalog_id, r->model, &e) != 0)
        return -1;
    return catalog_price(&e, r->u.input_tokens, r->u.output_tokens, r->u.cached_tokens,
                         r->u.cache_write_tokens, r->u.cache_write_1h_tokens, split);
}

double spend_total(const struct spend_totals *t, int *approx)
{
    double est = 0;
    for (size_t i = 0; i < t->n_recs; i++) {
        double c = spend_rec_price(&t->recs[i], NULL);
        if (c >= 0)
            est += c;
    }
    /* Every record exists because its response reported no cost, so any
     * record at all makes the figure inexact — whether it priced to a
     * positive estimate, to zero (zero catalog rates are still an
     * estimate), or not at all. A reported subtotal must never display
     * as an exact grand total. */
    if (approx)
        *approx = t->n_recs > 0;
    return t->reported + est;
}

int spend_unpriced(const struct spend_totals *t)
{
    for (size_t i = 0; i < t->n_recs; i++)
        if (spend_rec_price(&t->recs[i], NULL) < 0)
            return 1;
    return 0;
}

int spend_split(const struct spend_totals *t, struct catalog_split *out)
{
    *out = (struct catalog_split){0};
    int priced = 0;
    for (size_t i = 0; i < t->n_recs; i++) {
        struct catalog_split s;
        if (spend_rec_price(&t->recs[i], &s) < 0)
            continue;
        out->in += s.in;
        out->cache_read += s.cache_read;
        out->cache_write += s.cache_write;
        out->out += s.out;
        priced = 1;
    }
    return priced;
}

void spend_free(struct spend_totals *t)
{
    for (size_t i = 0; i < t->n_recs; i++) {
        free(t->recs[i].catalog_id);
        free(t->recs[i].model);
    }
    free(t->recs);
    memset(t, 0, sizeof(*t));
}

int usage_reported(const struct stream_usage *u)
{
    return u->input_tokens >= 0 || u->output_tokens >= 0 || u->cost >= 0;
}

struct turn_usage *turn_usage_make(const struct stream_usage *u, long elapsed_ms,
                                   const char *catalog_id, const char *model)
{
    if (!usage_reported(u) && elapsed_ms < 0)
        return NULL;
    struct turn_usage *tu = xmalloc(sizeof(*tu));
    tu->usage = *u;
    tu->elapsed_ms = elapsed_ms;
    tu->cost_in = tu->cost_cache_read = tu->cost_cache_write = tu->cost_out = -1;
    tu->cost_total = -1;
    tu->cost_estimated = 0;
    if (u->cost >= 0) {
        tu->cost_total = u->cost;
        return tu;
    }
    if (!usage_reported(u))
        return tu; /* duration-only — no tokens to price, and a $0
                    * "estimate" would fabricate certainty about a
                    * request whose usage is simply unknown */
    if (!catalog_id || !*catalog_id || !model || !*model)
        return tu;
    struct catalog_entry e;
    if (catalog_lookup(catalog_id, model, &e) != 0)
        return tu;
    struct catalog_split split;
    double total = catalog_price(&e, u->input_tokens, u->output_tokens, u->cached_tokens,
                                 u->cache_write_tokens, u->cache_write_1h_tokens, &split);
    if (total < 0)
        return tu;
    tu->cost_total = total;
    tu->cost_in = split.in;
    tu->cost_cache_read = split.cache_read;
    tu->cost_cache_write = split.cache_write;
    tu->cost_out = split.out;
    tu->cost_estimated = 1;
    return tu;
}

static char *resolve_model_label(struct provider *p, const char *model)
{
    if (!model)
        return NULL;
    return (p && p->model_label) ? p->model_label(p, model) : xstrdup(model);
}

/* Publish the effective selection for subagent inheritance (see
 * bash_export_selection). The provider is exported by its resolvable id —
 * the "provider" config key the autoselector/selectors record — not the
 * display name, which HAX_PROVIDER_NAME can change to something
 * provider_find wouldn't accept; the factory name is only the last-ditch
 * fallback. Sited here (init + reconfigure) because these are the only two
 * places the session's model/effort are resolved, so every path — startup,
 * oneshot, /provider, /model, /effort, /preset — republishes for free. */
static void export_selection(const struct provider *p, const struct agent_session *s)
{
    const char *id = config_str("provider");
    if ((!id || !*id) && p)
        id = p->name;
    bash_export_selection(id, s->model, s->effort);
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
    s->model_label = resolve_model_label(p, s->model);
    s->provider_name = p ? p->name : NULL;

    /* --raw collapses to "no system message + no tools advertised" so the
     * model sees only the user text. HAX_SYSTEM_PROMPT="" remains the
     * narrower opt-out (no system message but tools stay). */
    s->raw = opts->raw;
    s->sys = build_system_prompt(s->model_label, opts->raw);
    const char *effort = resolve_effort(p);
    s->effort = effort ? xstrdup(effort) : NULL;

    s->n_tools = opts->raw ? 0 : N_TOOLS;
    if (s->n_tools) {
        s->tools = xmalloc(s->n_tools * sizeof(*s->tools));
        for (size_t i = 0; i < s->n_tools; i++)
            s->tools[i] = TOOLS[i]->def;
    }
    export_selection(p, s);
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
    char *new_model = xstrdup(model);
    char *new_model_label = resolve_model_label(p, new_model);
    free(s->model);
    free(s->model_label);
    s->model = new_model;
    s->model_label = new_model_label;
    s->provider_name = p->name;
    /* Rebuild the system prompt so its Environment section names the new model.
     * Tools and history are deliberately untouched — a switch keeps the
     * conversation going under the new settings. */
    free(s->sys);
    s->sys = build_system_prompt(s->model_label, s->raw);
    const char *effort = resolve_effort(p);
    free(s->effort);
    s->effort = effort ? xstrdup(effort) : NULL;
    export_selection(p, s);
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
    free(s->model_label);
    free(s->effort);
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
        .effort = s->effort,
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

/* True when `it` is a tool_result whose output already ends with the
 * interrupt marker — i.e. bash appended its "[interrupted]" footer when
 * killed by user-Esc. Non-bash tools (read, write, edit) finish normally
 * without any footer, so their results never read as marked. */
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

void agent_session_mark_interrupt(struct agent_session *s)
{
    /* Look past inert trailing items (usage footers, boundaries) to the
     * last *content* item — footer emission between the tool batch and
     * this check must not hide an already-marked result and provoke a
     * duplicate marker. */
    size_t i = s->n_items;
    while (i > 0 &&
           (s->items[i - 1].kind == ITEM_TURN_USAGE || s->items[i - 1].kind == ITEM_TURN_BOUNDARY))
        i--;
    if (i > 0 && tool_result_is_marked(&s->items[i - 1]))
        return;
    items_append(&s->items, &s->n_items, &s->cap_items,
                 (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup(INTERRUPT_MARKER)});
}

void agent_session_add_turn_usage(struct agent_session *s, const struct provider *p,
                                  const struct stream_usage *u, long elapsed_ms)
{
    struct turn_usage *tu = turn_usage_make(u, elapsed_ms, p ? p->catalog_id : NULL, s->model);
    if (!tu)
        return;
    items_append(&s->items, &s->n_items, &s->cap_items,
                 (struct item){
                     .kind = ITEM_TURN_USAGE,
                     .usage = tu,
                     .provider = s->provider_name ? xstrdup(s->provider_name) : NULL,
                     .model = s->model && *s->model ? xstrdup(s->model) : NULL,
                 });
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
