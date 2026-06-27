/* SPDX-License-Identifier: MIT */
#include "select.h"

#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "config.h"
#include "provider.h"
#include "providers/registry.h"
#include "render/render_ctx.h"
#include "util.h"
#include "system/bg_job.h"
#include "terminal/picker.h"
#include "terminal/ui.h"

/* ---------- parallel availability probe ---------- */

/* One factory's availability check, run on its own worker thread so a
 * local-server probe (bounded by http_get's connect timeout) doesn't
 * serialize behind the others. Writes into result slots that outlive the
 * join; frees its own arg per the bg_job contract. */
struct avail_arg {
    const struct provider_factory *f;
    int *avail;
    const char **reason;
};

static void avail_worker(struct bg_job *job, void *arg)
{
    (void)job;
    struct avail_arg *a = arg;
    const char *reason = NULL;
    int ok = a->f->available ? a->f->available(a->f->name, &reason) : 1; /* no hook ⇒ available */
    *a->avail = ok;
    *a->reason = ok ? NULL : (reason ? reason : "unavailable");
    free(a);
}

/* Probe every factory's availability concurrently and collect the verdicts
 * into avail[]/reason[]. Total wait is about one probe's worth of time (the
 * checks run in parallel and each self-bounds), so opening the picker stays
 * fast even when a configured local server is unreachable. */
static void probe_availability(const struct provider_factory *const *facs, size_t n, int *avail,
                               const char **reason)
{
    struct bg_job **jobs = xcalloc(n, sizeof(*jobs));
    for (size_t i = 0; i < n; i++) {
        avail[i] = 1; /* default if the spawn fails: assume selectable */
        reason[i] = NULL;
        struct avail_arg *a = xmalloc(sizeof(*a));
        a->f = facs[i];
        a->avail = &avail[i];
        a->reason = &reason[i];
        jobs[i] = bg_job_spawn(avail_worker, a);
        if (!jobs[i])
            free(a); /* worker never ran; leave the default-available slot */
    }
    for (size_t i = 0; i < n; i++)
        bg_job_join(jobs[i]); /* NULL is a no-op */
    free(jobs);
}

/* Cold-start provider pick when the user hasn't configured one: the single
 * "start on something sensible" path. Returns a constructed provider, or NULL
 * when nothing is available (the caller then opens the REPL provider-less,
 * with a banner pointing at /provider).
 *
 * Priority: the built-in default first, then the rest. The pick is NOT
 * persisted — it's an inference for this run, so once the user's real default
 * becomes usable again it transparently comes back; an explicit /model or
 * /effort is what promotes the inference to a stored choice (it pins the
 * provider alongside). */
struct provider *provider_autoselect(void)
{
    /* The default (highest-priority provider) is cheap to check — codex reads
     * a local auth file — so try it first and short-circuit before the
     * parallel probe: the common logged-in start then stays instant. No note:
     * the default is what the user expects, not a surprising swap. */
    const struct provider_factory *def = provider_default();
    if (def && (!def->available || def->available(def->name, NULL))) {
        struct provider *p = def->new(def->name);
        if (p) {
            /* Record the pick as a session-scoped override (NOT persisted),
             * so config_str("provider") names the live provider for the rest
             * of the run — that's the resolvable id (factory name / future
             * config-defined name) an explicit /model or /effort then pins.
             * Leaving it unpersisted is deliberate: with no follow-up action
             * the real default transparently returns next launch. */
            config_set_override("provider", def->name);
            return p;
        }
    }

    /* Default unusable: probe the rest in parallel (bounded, so the list
     * stays responsive even when a local server hangs) and start on the first
     * available one in priority order. That order is why a configured
     * HAX_OPENAI_BASE_URL lands on openai-compatible rather than llama.cpp /
     * ollama (which honor the same override but rank lower). No note — the
     * banner that prints next already shows the active provider, and the
     * "why" lives in /provider, where the default shows dimmed with a reason. */
    size_t n = 0;
    const struct provider_factory *const *facs = provider_all(&n);
    int *avail = xmalloc(n * sizeof(*avail));
    const char **reason = xmalloc(n * sizeof(*reason));
    probe_availability(facs, n, avail, reason);

    struct provider *p = NULL;
    for (size_t i = 0; i < n && !p; i++) {
        if (facs[i] == def || !avail[i])
            continue; /* default already tried above */
        /* available() is a pre-check, not a guarantee — construction can
         * still fail (a server that dropped between probe and connect); on
         * that race just try the next available one. */
        p = facs[i]->new(facs[i]->name);
        if (p)
            config_set_override("provider", facs[i]->name); /* see codex branch above */
    }
    free(avail);
    free(reason);
    return p;
}

/* ---------- model / effort pick steps ---------- */

/* Run the model picker for `p` (which need not be the live provider — the
 * /provider flow picks against the prospective new one before committing).
 * Returns a malloc'd model id, or NULL on cancel or when the provider can't
 * enumerate models (a note is printed in the latter case). `cur` marks the
 * row currently in use, if it appears in the list. */
static char *choose_model(struct agent_state *st, struct provider *p, const char *cur)
{
    const char *name = p->name ? p->name : "?";
    char **ids = NULL;
    size_t n = 0;

    /* Distinguish the three "no menu" cases so the note is actionable: the
     * provider can't enumerate at all, the fetch failed (server down /
     * unreachable), or the catalog came back empty (e.g. ollama with
     * nothing pulled). The last is a normal state, not an error. */
    if (!p->list_models) {
        ui_note("%s can't list models — set one with HAX_MODEL or in config", name);
        st->r->disp.trail = 1;
        return NULL;
    }
    if (p->list_models(p, &ids, &n) != 0) {
        ui_note("could not reach %s to list models — is it running?", name);
        st->r->disp.trail = 1;
        free(ids);
        return NULL;
    }
    if (n == 0) {
        /* Reachable but empty. The remedy is backend-specific (ollama: pull
         * one; llama.cpp: load one; a proxy: configure one), so state the
         * fact without prescribing a fix the provider may not support. */
        ui_note("%s has no models available", name);
        st->r->disp.trail = 1;
        free(ids);
        return NULL;
    }
    if (n == 1) {
        /* A single model isn't a choice — the common case for a single-model
         * llama.cpp / ollama server. Skip the picker and use it directly; the
         * post-switch confirmation still shows which model is now active. */
        char *only = xstrdup(ids[0]);
        free(ids[0]);
        free(ids);
        return only;
    }

    struct picker_item *items = xmalloc(n * sizeof(*items));
    for (size_t i = 0; i < n; i++) {
        items[i].label = ids[i];
        items[i].disabled = 0;
        items[i].detail = (cur && strcmp(ids[i], cur) == 0) ? "current" : NULL;
    }
    struct picker_opts opts = {.title = "select a model", .items = items, .n = n};
    long sel = picker_run(&opts);
    char *chosen = (sel >= 0) ? xstrdup(ids[sel]) : NULL;

    free(items);
    for (size_t i = 0; i < n; i++)
        free(ids[i]);
    free(ids);
    return chosen;
}

/* Run the effort picker for `p`. Returns 1 when a choice was made (including
 * "default"), 0 on cancel or when the provider has no effort ladder. On a
 * made choice *out is set: NULL for "default" (clear the pick → provider /
 * config default), or a malloc'd effort value.
 *
 * `announce` controls the no-ladder note: print it only when the user asked
 * for effort directly (/effort), not when this runs as the automatic tail
 * of /model or /provider — there, a provider without effort levels should
 * just be skipped silently. */
static int choose_effort(struct agent_state *st, struct provider *p, const char *cur, char **out,
                         int announce)
{
    *out = NULL;
    const char *const *eff = NULL;
    size_t n = p->list_efforts ? p->list_efforts(p, &eff) : 0;
    if (n == 0) {
        if (announce) {
            ui_note("the %s provider doesn't expose reasoning-effort levels",
                    p->name ? p->name : "?");
            st->r->disp.trail = 1;
        }
        return 0;
    }

    struct picker_item *items = xmalloc((n + 1) * sizeof(*items));
    items[0].label = "default";
    items[0].detail = "let the provider choose";
    items[0].disabled = 0;
    for (size_t i = 0; i < n; i++) {
        items[i + 1].label = eff[i];
        items[i + 1].disabled = 0;
        items[i + 1].detail = (cur && strcmp(eff[i], cur) == 0) ? "current" : NULL;
    }
    struct picker_opts opts = {.title = "select reasoning effort", .items = items, .n = n + 1};
    long sel = picker_run(&opts);
    free(items);

    if (sel < 0)
        return 0; /* cancel — no change */
    /* sel == 0 is "default" (leave *out NULL → clear); sel > 0 is a value.
     * "default" differs from the ladder's "none": none sends none, default
     * sends nothing and lets the provider choose. */
    if (sel > 0)
        *out = xstrdup(eff[sel - 1]);
    return 1;
}

/* Record a selection into the live override + persisted state tiers. NULL
 * clears (resolution falls through to env / config / provider default). */
static void commit_selection(const char *key, const char *val)
{
    config_set_override(key, val);
    if (config_persist_state(key, val) != 0) {
        /* The override above keeps the pick active this session, but it didn't
         * reach state.json (an unwritable state dir), so it won't survive a
         * restart. Say so once — otherwise the user is left puzzled when
         * settings silently revert next launch. Guarded because one pick
         * commits up to three keys (provider/model/effort). */
        static int warned;
        if (!warned) {
            warned = 1;
            ui_note("couldn't save to state.json — this choice applies to this session only");
        }
    }
}

/* The resolvable id of the live provider: the "provider" config key that
 * provider_find (and, eventually, config-defined custom providers) consume —
 * NOT the display name, which HAX_PROVIDER_NAME or a custom provider can
 * change and which wouldn't resolve. autoselect records its pick under this
 * key (as a session override) and /provider commits it, so it's populated
 * whenever a provider is live; the display name is only a last-ditch
 * fallback. Returns malloc'd; caller frees. */
static char *current_provider_id(const struct provider *p)
{
    const char *id = config_str("provider");
    if (id && *id)
        return xstrdup(id);
    return xstrdup(p && p->name ? p->name : "");
}

/* ---------- public flows ---------- */

void select_effort(struct agent_state *st)
{
    struct provider *p = (struct provider *)st->provider;
    if (!p) {
        ui_note("no provider selected — use /provider to choose one first");
        st->r->disp.trail = 1;
        return;
    }
    char *e;
    /* Explicit /effort: announce when the provider has no effort levels. */
    if (!choose_effort(st, p, st->sess->reasoning_effort, &e, 1))
        return;
    /* Pin the provider alongside the effort, for the same reason select_model
     * does: an explicit setting against an auto-selected provider should
     * promote the whole set to a stored choice. Idempotent when already
     * pinned. */
    char *pid = current_provider_id(p);
    commit_selection("provider", pid);
    free(pid);
    /* "default" row → the sentinel (use the provider's default, shadowing a
     * lower-tier env/config effort), not a delete (which would let it leak). */
    commit_selection("reasoning_effort", e ? e : CONFIG_VALUE_DEFAULT);
    free(e);
    agent_apply_settings(st);
}

void select_model(struct agent_state *st)
{
    /* Standalone: a cancelled model pick aborts with no change. A made pick
     * chains into effort (also cancellable) and then applies once. */
    struct provider *p = (struct provider *)st->provider;
    if (!p) {
        ui_note("no provider selected — use /provider to choose one first");
        st->r->disp.trail = 1;
        return;
    }
    char *m = choose_model(st, p, st->sess->model);
    if (!m)
        return;

    /* Run the chained effort pick BEFORE committing anything. commit_selection
     * persists into the state tier, which deep-copies and frees the old tier
     * object — and st->sess->reasoning_effort may point into it (after a prior
     * session's /effort). Reading it as the picker's "current" marker after a
     * commit would be a use-after-free, so gather both picks first, then commit
     * together. Skips silently if this provider has no ladder. */
    char *e = NULL;
    int chose_effort = choose_effort(st, p, st->sess->reasoning_effort, &e, 0);

    /* Pin the provider alongside the model. Without this, a model picked
     * against an auto-selected (and thus unpinned) provider would leave
     * `provider` unset, so the next launch would re-auto-select and warn
     * despite the user having explicitly configured a model. Idempotent when
     * the provider is already pinned to the same value. */
    char *pid = current_provider_id(p);
    commit_selection("provider", pid);
    free(pid);
    commit_selection("model", m);
    free(m);
    if (chose_effort) {
        /* "default" row → sentinel, not delete (see select_effort). */
        commit_selection("reasoning_effort", e ? e : CONFIG_VALUE_DEFAULT);
        free(e);
    }
    agent_apply_settings(st);
}

void select_provider(struct agent_state *st)
{
    size_t n = 0;
    const struct provider_factory *const *facs = provider_all(&n);

    int *avail = xmalloc(n * sizeof(*avail));
    const char **reason = xmalloc(n * sizeof(*reason));
    probe_availability(facs, n, avail, reason);

    /* Mark the live provider's row "current". Compare against its resolvable
     * id (the "provider" config key the rows are keyed by), not p->name —
     * HAX_PROVIDER_NAME / a custom provider can rename the display name so it
     * no longer equals any factory id, which would leave the row unmarked and
     * make re-selecting it rebuild instead of continuing into /model. NULL
     * when no provider constructed at startup; then nothing is current. */
    char *cur = st->provider ? current_provider_id(st->provider) : NULL;
    struct picker_item *items = xmalloc(n * sizeof(*items));
    for (size_t i = 0; i < n; i++) {
        items[i].label = facs[i]->name;
        items[i].disabled = !avail[i];
        if (!avail[i])
            items[i].detail = reason[i] ? reason[i] : "unavailable";
        else
            items[i].detail = (cur && strcmp(facs[i]->name, cur) == 0) ? "current" : NULL;
    }
    struct picker_opts opts = {.title = "select a provider", .items = items, .n = n};
    long sel = picker_run(&opts);

    const struct provider_factory *f = (sel >= 0) ? facs[sel] : NULL;
    free(items);
    free(avail);
    free(reason);
    if (!f) {
        free(cur);
        return; /* cancelled / non-tty — leave disp as the dispatcher's separator */
    }

    /* Re-picking the current provider just continues into model/effort,
     * with standalone semantics (a cancelled model pick aborts), rather
     * than tearing down and rebuilding the same connection. */
    if (cur && strcmp(f->name, cur) == 0) {
        free(cur);
        select_model(st);
        return;
    }

    /* Construct the new provider before touching any state. Some constructors
     * mutate the override tier as a side effect (llama.cpp's probe reconciles
     * the served model into a "model" override), so snapshot it first: every
     * abort path below restores it, leaving the still-current provider's
     * overrides untouched. On the commit path the snapshot is just discarded —
     * commit_selection sets the real values. On failure the factory has
     * already explained why (no key, server down …); the current provider
     * stays live. */
    struct config_override_state *ov = config_override_snapshot();
    struct provider *newp = f->new(f->name);
    if (!newp) {
        config_override_restore(ov);
        st->r->disp.trail = 1; /* the factory printed a raw error line */
        free(cur);
        return;
    }

    /* Choose model and effort *against the new provider* but DON'T commit
     * the switch yet — so an unusable result (no model, and no default to
     * fall back on) leaves the current provider untouched instead of
     * stranding the session on a half-configured backend. The new provider
     * isn't live during these picks (choose_* take it explicitly), so its
     * list_models/list_efforts drive the menus correctly. */
    char *m = choose_model(st, newp, NULL);
    int has_default = newp->default_model && *newp->default_model;
    if (!m && !has_default) {
        ui_note("staying on %s — no model chosen for %s", cur ? cur : "?", f->name);
        st->r->disp.trail = 1;
        newp->destroy(newp);
        config_override_restore(ov);
        free(cur);
        return;
    }
    config_override_state_free(ov); /* committing below — keep the side effects */
    /* Pick effort against newp (cur=NULL). A switch always resets effort: to
     * the chosen value, or — for the "default" row, a no-ladder provider, or a
     * cancel — to newp's own default via the sentinel. So only whether an
     * explicit value came back matters, not the made/skipped return. */
    char *e = NULL;
    choose_effort(st, newp, NULL, &e, 0);

    /* Commit: a provider switch invalidates the old model/effort (they belong
     * to the old backend), so set the new model and effort to the picked value
     * or the sentinel — "use newp's default", which shadows a stale lower-tier
     * env/config value instead of leaking it into the new provider — then
     * record the provider, swap, and apply. */
    commit_selection("provider", f->name);
    commit_selection("model", m ? m : CONFIG_VALUE_DEFAULT);
    commit_selection("reasoning_effort", e ? e : CONFIG_VALUE_DEFAULT);

    agent_set_provider(st, newp);
    agent_apply_settings(st);

    free(cur);
    free(m);
    free(e);
}
