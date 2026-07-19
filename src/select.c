/* SPDX-License-Identifier: MIT */
#include "select.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "agent.h"
#include "busy.h"
#include "config.h"
#include "provider.h"
#include "providers/registry.h"
#include "render/render_ctx.h"
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

static int cmp_model_id(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Outcome of one picker step. The chained flows treat these differently:
 * cancel aborts the whole command with nothing changed, "no picker to
 * show" continues on the provider's defaults, and a failure also rolls a
 * provider switch back. */
enum pick_status {
    PICK_MADE,      /* a choice was made; the value out-param is set */
    PICK_CANCELLED, /* user cancelled the picker (Escape / non-tty) */
    PICK_NONE,      /* no picker to show: can't enumerate / no ladder */
    PICK_FAILED,    /* enumeration failed — provider looks unusable now */
};

/* Run the model picker for `p` (which need not be the live provider — the
 * /provider flow picks against the prospective new one before committing).
 * Returns a malloc'd model id iff *status is PICK_MADE, else NULL with
 * *status saying why (a note/error was already printed for NONE/FAILED).
 * `cur` marks the row currently in use, if it appears in the list. */
static char *choose_model(struct agent_state *st, struct provider *p, const char *cur,
                          enum pick_status *status)
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
        *status = PICK_NONE;
        return NULL;
    }
    /* The catalog fetch blocks the foreground for up to the adapter's
     * timeout (OpenRouter's list runs to hundreds of entries): run it
     * under a busy window so a spinner shows and Esc cancels during the
     * fetch, not just in the picker. */
    struct busy *b = busy_begin("fetching models...");
    char *err = NULL;
    int rc = p->list_models(p, &ids, &n, &err, busy_tick, NULL);
    if (busy_end(b)) {
        /* Esc during the fetch aborts like Esc in the picker; a result
         * that landed in the same instant is discarded, not committed.
         * busy_end printed the [interrupted] marker — mark the trail
         * like the other printed-note exits here. */
        for (size_t i = 0; i < n; i++)
            free(ids[i]);
        free(ids);
        free(err);
        st->r->disp.trail = 1;
        *status = PICK_CANCELLED;
        return NULL;
    }
    if (rc != 0) {
        /* The adapter's diagnostic names the endpoint and remedy ("codex
         * token expired — …"); the bare fallback only covers an adapter
         * that left *err unset. Red — same class as a failed /usage fetch. */
        if (err)
            ui_error("%s", err);
        else
            ui_error("failed to list models for %s", name);
        st->r->disp.trail = 1;
        *status = PICK_FAILED;
        free(err);
        free(ids);
        return NULL;
    }
    if (n == 0) {
        /* Reachable but empty. The remedy is backend-specific (ollama: pull
         * one; llama.cpp: load one; a proxy: configure one), so state the
         * fact without prescribing a fix the provider may not support. */
        ui_note("%s has no models available", name);
        st->r->disp.trail = 1;
        *status = PICK_FAILED;
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
        *status = PICK_MADE;
        return only;
    }

    /* The provider declares whether its catalog order is worth preserving
     * (sort_models); the global sort_models key is a tri-state user override on
     * top of that per-provider default — on/off force it, auto (and unset)
     * defer. config_bool_or resolves exactly that, and /config's tri-state
     * validation accepts the same grammar, so the two agree. */
    if (config_bool_or("sort_models", p->sort_models))
        qsort(ids, n, sizeof(*ids), cmp_model_id);

    struct picker_item *items = xcalloc(n, sizeof(*items));
    size_t initial = 0;
    for (size_t i = 0; i < n; i++) {
        items[i].label = ids[i];
        items[i].detail = NULL;
        items[i].dim = 0;
        items[i].current = cur && strcmp(ids[i], cur) == 0;
        if (items[i].current)
            initial = i;
    }
    struct picker_opts opts = {
        .title = "select a model", .items = items, .n = n, .initial = initial};
    long sel = picker_run(&opts);
    char *chosen = (sel >= 0) ? xstrdup(ids[sel]) : NULL;
    *status = chosen ? PICK_MADE : PICK_CANCELLED;

    free(items);
    for (size_t i = 0; i < n; i++)
        free(ids[i]);
    free(ids);
    return chosen;
}

/* Run the effort picker for `p`. On PICK_MADE *out is set: NULL for the
 * "default" row (clear the pick → provider / config default), or a malloc'd
 * effort value. PICK_NONE = no effort ladder, PICK_CANCELLED = Escape.
 *
 * `announce` prints the no-ladder note only when the user asked for effort
 * directly (/effort); the chained tails of /model and /provider skip a
 * no-ladder provider silently. */
static enum pick_status choose_effort(struct agent_state *st, struct provider *p, const char *cur,
                                      char **out, int announce)
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
        return PICK_NONE;
    }

    struct picker_item *items = xcalloc((n + 1), sizeof(*items));
    items[0].label = "default";
    items[0].detail = "let the provider choose";
    items[0].dim = 0;
    items[0].current = 0;
    size_t initial = 0;
    for (size_t i = 0; i < n; i++) {
        items[i + 1].label = eff[i];
        items[i + 1].detail = NULL;
        items[i + 1].dim = 0;
        items[i + 1].current = cur && strcmp(eff[i], cur) == 0;
        if (items[i + 1].current)
            initial = i + 1;
    }
    struct picker_opts opts = {
        .title = "select reasoning effort", .items = items, .n = n + 1, .initial = initial};
    long sel = picker_run(&opts);
    free(items);

    if (sel < 0)
        return PICK_CANCELLED;
    /* sel == 0 is "default" (leave *out NULL → clear); sel > 0 is a value.
     * "default" differs from the ladder's "none": none sends none, default
     * sends nothing and lets the provider choose. */
    if (sel > 0)
        *out = xstrdup(eff[sel - 1]);
    return PICK_MADE;
}

/* Record a selection: session overrides for the members actually picked, and
 * one atomic state-tier write for the whole set. NULL model/effort means "not
 * picked here" — config_persist_selection keeps the stored value when the
 * provider is unchanged and resets it to the sentinel when the commit re-pins
 * a different one (e.g. an /effort pick promoting a one-off HAX_PROVIDER run
 * must not carry the old provider's saved model along). */
static void commit_selection(const char *provider, const char *model, const char *effort)
{
    /* An explicit pick exits any active preset stance — atomically, not as
     * a blend: the stance name is cleared (the banner must not keep
     * claiming a preset whose selection was just overridden) along with
     * the preset's system_prompt override, so the regular prompt returns.
     * Presets are the only writer of that override, so clearing is
     * unambiguous; every commit path follows with agent_apply_settings,
     * which rebuilds the prompt. config_persist_selection below removes
     * the persisted name in the same write. The name is shadowed with the
     * empty sentinel, not deleted: a delete would let a lower-tier name
     * (HAX_PRESET, a config-file default, or state if its write fails)
     * resurface in the banner as a stance that is no longer applied. */
    config_set_override("preset", "");
    config_set_override("system_prompt", NULL);
    config_set_override("provider", provider);
    if (model)
        config_set_override("model", model);
    if (effort)
        config_set_override("effort", effort);
    if (config_persist_selection(provider, model, effort) != 0) {
        /* The overrides above keep the pick active this session, but it didn't
         * reach state.json (an unwritable state dir), so it won't survive a
         * restart. Say so once — otherwise the user is left puzzled when
         * settings silently revert next launch. */
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

/* A constructor may diagnose directly through hax_err/hax_warn on stderr.
 * Resynchronize disp with the fresh terminal row that output left behind so
 * the next stdout block still gets its separator. */
static void sync_constructor_diagnostics(struct agent_state *st, unsigned long before)
{
    if (hax_diag_sequence() != before) {
        st->r->disp.held = 0;
        st->r->disp.trail = 1;
    }
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
    /* Explicit /effort: announce when the provider has no effort levels.
     * Only a made pick commits; cancel and no-ladder both change nothing. */
    if (choose_effort(st, p, st->sess->effort, &e, 1) != PICK_MADE)
        return;
    /* The provider is pinned alongside the effort, for the same reason
     * select_model does: an explicit setting against an auto-selected provider
     * should promote the whole set to a stored choice. Idempotent when already
     * pinned. The "default" row → the sentinel (use the provider's default,
     * shadowing a lower-tier env/config effort), not a delete (which would let
     * it leak); the model isn't picked here, so NULL — kept unless the pin
     * changes the stored provider.
     *
     * Exiting a preset stance is the exception to the NULL model: the
     * session keeps running the preset's model (an effort pick shouldn't
     * change the model), but NULL would persist "keep the previously
     * *stored* model" — the pre-preset one — and the next launch would
     * diverge from what this pick left running. Materialize the stance's
     * effective model into the replacement selection instead. Read before
     * commit_selection clears the stance. */
    const char *stance = config_str("preset");
    const char *eff_model =
        (stance && *stance && st->sess->model && *st->sess->model) ? st->sess->model : NULL;
    char *pid = current_provider_id(p);
    commit_selection(pid, eff_model, e ? e : CONFIG_VALUE_DEFAULT);
    free(pid);
    free(e);
    agent_apply_settings(st, p);
}

void select_model(struct agent_state *st)
{
    /* Escape anywhere in the chain (model picker or the chained effort
     * picker) aborts the whole /model with no change; a completed chain
     * applies once. */
    struct provider *p = (struct provider *)st->provider;
    if (!p) {
        ui_note("no provider selected — use /provider to choose one first");
        st->r->disp.trail = 1;
        return;
    }
    enum pick_status ms;
    char *m = choose_model(st, p, st->sess->model, &ms);
    if (!m)
        return; /* cancelled or no menu — notes already printed */

    /* Run the chained effort pick BEFORE committing. commit_selection persists
     * into the state tier, which deep-copies and frees the old tier object —
     * and st->sess->effort may point into it (after a prior
     * session's /effort). Reading it as the picker's "current" marker after a
     * commit would be a use-after-free, so gather both picks first, then commit
     * once. Skips silently if this provider has no ladder. */
    char *e = NULL;
    enum pick_status es = choose_effort(st, p, st->sess->effort, &e, 0);
    if (es == PICK_CANCELLED) {
        /* Escape mid-chain: discard the model pick too — nothing commits. */
        free(m);
        return;
    }

    /* The provider is pinned alongside the model — otherwise a model picked
     * against an auto-selected (unpinned) provider would make the next
     * launch re-auto-select and warn. Effort commits as the picked value,
     * or the sentinel for both the "default" row and a no-ladder skip: a
     * provider without an effort ladder shouldn't be sent one, so the pin
     * resets to "provider default" instead of keeping (or letting a
     * lower-tier env/config value leak into) an effort it never advertised.
     * Same resolution /provider applies on a switch. */
    char *pid = current_provider_id(p);
    commit_selection(pid, m, e ? e : CONFIG_VALUE_DEFAULT);
    free(pid);
    free(m);
    free(e);
    agent_apply_settings(st, p);
}

static int cmp_factory_name(const void *a, const void *b)
{
    const struct provider_factory *const *fa = a;
    const struct provider_factory *const *fb = b;
    return strcmp((*fa)->name, (*fb)->name);
}

void select_provider(struct agent_state *st)
{
    size_t n = 0;
    const struct provider_factory *const *all = provider_all(&n);

    /* Display order is alphabetical, not registry priority: priority drives
     * cold-start autoselect, but in a picker predictable lookup wins as the
     * list grows. */
    const struct provider_factory **facs = xmalloc(n * sizeof(*facs));
    memcpy(facs, all, n * sizeof(*facs));
    qsort(facs, n, sizeof(*facs), cmp_factory_name);

    /* Esc is not observed during the pre-pick phases — this probe, the
     * dim-row recheck at commit, provider construction. All are bounded by
     * the short probe/connect timeouts (a couple of seconds worst case),
     * so cancellation plumbing isn't worth it there; Esc coverage starts
     * with the pickers and the busy-window catalog fetch. */
    int *avail = xmalloc(n * sizeof(*avail));
    const char **reason = xmalloc(n * sizeof(*reason));
    probe_availability(facs, n, avail, reason);

    /* Mark the live provider's row "current". Compare against its resolvable
     * id (the "provider" config key the rows are keyed by), not p->name —
     * HAX_PROVIDER_NAME / a custom provider can rename the display name so it
     * no longer equals any factory id, which would leave the row unmarked and
     * make re-selecting it rebuild instead of continuing into /model. NULL
     * when no provider constructed at startup; then nothing is current.
     *
     * Unavailable rows are dim-with-a-reason but still selectable: the probe
     * is advisory (and possibly stale by the time the user commits), and a
     * navigable row is the only way a long tail of unavailable providers
     * stays readable at all — the viewport follows the selection. */
    char *cur = st->provider ? current_provider_id(st->provider) : NULL;
    struct picker_item *items = xcalloc(n, sizeof(*items));
    size_t initial = 0;
    for (size_t i = 0; i < n; i++) {
        items[i].label = facs[i]->name;
        items[i].dim = !avail[i];
        items[i].detail = avail[i] ? NULL : (reason[i] ? reason[i] : "unavailable");
        items[i].current = cur && strcmp(facs[i]->name, cur) == 0;
        if (items[i].current)
            initial = i;
    }
    struct picker_opts opts = {
        .title = "select a provider", .items = items, .n = n, .initial = initial};
    long sel = picker_run(&opts);

    const struct provider_factory *f = (sel >= 0) ? facs[sel] : NULL;
    int sel_avail = (sel >= 0) ? avail[sel] : 1;
    free(items);
    free(avail);
    free(reason);
    free(facs);
    if (!f) {
        free(cur);
        return; /* cancelled / non-tty — leave disp as the dispatcher's separator */
    }

    /* A dim row was picked: the open-time probe said unavailable, but that
     * verdict may be stale, so re-check now. Still unavailable → report the
     * exact reason and stay on the current provider; a fresh pass (a server
     * that came up since) proceeds with the normal switch. */
    if (!sel_avail && f->available) {
        const char *why = NULL;
        if (!f->available(f->name, &why)) {
            ui_note("%s is unavailable — %s", f->name, why ? why : "unavailable");
            st->r->disp.trail = 1;
            free(cur);
            return;
        }
    }

    /* Re-picking the current provider just continues into model/effort,
     * with standalone semantics (a cancelled model pick aborts), rather
     * than tearing down and rebuilding the same connection. */
    if (cur && strcmp(f->name, cur) == 0) {
        free(cur);
        select_model(st);
        return;
    }

    /* Construct transactionally under the prospective selection. A provider
     * switch invalidates the old backend's model/effort, so shadow them with
     * the default sentinel before construction; otherwise value-dependent
     * constructors (notably llama.cpp's live-model reconciliation) mistake the
     * outgoing provider's exact model for intent against the new backend.
     * Every abort restores the snapshot; commit_selection replaces these
     * provisional values with the actual picks. */
    struct config_override_state *ov = config_override_snapshot();
    config_set_override("provider", f->name);
    config_set_override("model", CONFIG_VALUE_DEFAULT);
    config_set_override("effort", CONFIG_VALUE_DEFAULT);
    unsigned long diag_before = hax_diag_sequence();
    struct provider *newp = f->new(f->name);
    sync_constructor_diagnostics(st, diag_before);
    if (!newp) {
        config_override_restore(ov);
        st->r->disp.trail = 1; /* the factory printed a raw error line */
        free(cur);
        return;
    }

    /* Choose model and effort *against the new provider* but DON'T commit
     * yet: Escape at either picker or an unusable backend must leave the
     * current provider untouched. Only PICK_NONE (no enumerate hook — no
     * picker shown, nothing cancelled) falls back to newp's default model;
     * cancel and failure roll the whole switch back. A cancel bails
     * silently — Esc is deliberate, and a killed fetch already left its
     * [interrupted] marker; the "staying on" note is for failures, where
     * it follows the red diagnostic. newp isn't live during these picks,
     * so choose_* take it explicitly. */
    enum pick_status ms;
    char *m = choose_model(st, newp, NULL, &ms);
    int has_default = newp->default_model && *newp->default_model;
    if (!m && (ms != PICK_NONE || !has_default)) {
        if (ms != PICK_CANCELLED) {
            ui_note("staying on %s — no model chosen for %s", cur ? cur : "?", f->name);
            st->r->disp.trail = 1;
        }
        newp->destroy(newp);
        config_override_restore(ov);
        free(cur);
        return;
    }
    /* Escape at the effort step aborts the switch too, silently (see
     * above). A completed switch resets effort to the pick, or — "default"
     * row / no ladder — to newp's own default via the sentinel. */
    char *e = NULL;
    if (choose_effort(st, newp, NULL, &e, 0) == PICK_CANCELLED) {
        newp->destroy(newp);
        config_override_restore(ov);
        free(cur);
        free(m);
        return;
    }

    /* Commit: a provider switch invalidates the old model/effort (they belong
     * to the old backend), so set the new model and effort to the picked value
     * or the sentinel — "use newp's default", which shadows a stale lower-tier
     * env/config value instead of leaking it into the new provider (passed
     * explicitly rather than as NULL: a same-provider re-pick must reset too,
     * not keep). Then swap and apply. */
    commit_selection(f->name, m ? m : CONFIG_VALUE_DEFAULT, e ? e : CONFIG_VALUE_DEFAULT);

    if (agent_apply_settings(st, newp) != 0) {
        newp->destroy(newp); /* ownership transfers only on success */
        config_override_restore(ov);
        free(cur);
        free(m);
        free(e);
        return;
    }
    config_override_state_free(ov); /* committed constructor side effects remain */

    free(cur);
    free(m);
    free(e);
}

void select_preset(struct agent_state *st, const char *name)
{
    char **names = NULL;
    size_t n = config_preset_names(&names);
    char *picked = NULL;

    if (!name) {
        if (n == 0) {
            ui_note("no presets defined — add a presets.<name> block to config.json");
            st->r->disp.trail = 1;
            goto out;
        }
        qsort(names, n, sizeof(*names), cmp_model_id); /* plain char* compare */
        struct picker_item *items = xcalloc(n, sizeof(*items));
        char **details = xcalloc(n, sizeof(*details)); /* owned detail strings */
        for (size_t i = 0; i < n; i++) {
            items[i].label = names[i];
            items[i].detail = config_preset_description(names[i]);
            items[i].dim = 0;
            items[i].current = 0;
            /* A provider name the registry can't resolve is a typo, not a
             * transient outage (availability is deliberately not probed
             * here) — show the row dim with the defect as its detail, like
             * the /provider picker's unavailable rows. Still selectable;
             * committing it reports the same error and changes nothing. */
            const char *prov = config_preset_provider(names[i]);
            if (!prov || !provider_find(prov)) {
                details[i] = xasprintf("unknown provider '%s'", prov ? prov : "?");
                items[i].detail = details[i];
                items[i].dim = 1;
            }
        }
        struct picker_opts opts = {
            .title = "select a preset", .items = items, .n = n, .initial = 0};
        long sel = picker_run(&opts);
        free(items);
        for (size_t i = 0; i < n; i++)
            free(details[i]);
        free(details);
        if (sel < 0)
            goto out; /* cancelled / non-tty */
        picked = xstrdup(names[sel]);
        name = picked;
    }

    /* Snapshot the override tier before applying: a preset that fails
     * validation or lands on an unusable setup must leave the session
     * exactly as it was. */
    struct config_override_state *ov = config_override_snapshot();
    char *err = NULL;
    if (config_preset_apply(name, &err) != 0) {
        ui_error("%s", err ? err : "preset failed to apply");
        free(err);
        config_override_restore(ov);
        st->r->disp.trail = 1;
        goto out;
    }

    /* The preset named a provider (config_preset_apply requires one).
     * Always construct it fresh under the applied overrides — even when the
     * id matches the live provider: construction is where value-dependent
     * behavior runs (llama.cpp reconciles the preset's model against the
     * live /v1/models, warning on a stale pick), so reusing the live
     * connection would let a same-provider preset bypass it. Strictly by
     * name, no picker chain: the preset carries the rest. */
    const char *after = config_str("provider");
    const struct provider_factory *f = provider_find(after);
    if (!f) {
        ui_error("preset '%s': unknown provider '%s'", name, after);
        config_override_restore(ov);
        st->r->disp.trail = 1;
        goto out;
    }
    unsigned long diag_before = hax_diag_sequence();
    struct provider *newp = f->new(f->name);
    sync_constructor_diagnostics(st, diag_before);
    if (!newp) {
        /* The factory printed the reason (no key, server down, …). */
        config_override_restore(ov);
        st->r->disp.trail = 1;
        goto out;
    }

    /* Gather-then-commit, like select_provider: a model must resolve for
     * the provider the preset lands on — checked BEFORE the old provider is
     * destroyed and the snapshot freed, so a preset that leaves no model
     * (omitted, provider has no default) rolls back to the previous
     * provider+model instead of committing a mismatched pair. Checked after
     * construction, which may have reconciled a discovered model into the
     * override tier (llama.cpp). Mirrors agent_session_reconfigure's own
     * test, so the agent_apply_settings below cannot fail it. */
    const char *m = config_str("model");
    if ((!m || !*m) && !(newp->default_model && *newp->default_model)) {
        ui_error("preset '%s': no model resolves for provider '%s' — name one in the preset", name,
                 after);
        newp->destroy(newp);
        config_override_restore(ov);
        st->r->disp.trail = 1;
        goto out;
    }

    /* Persist the stance like the other selectors, so the next launch
     * starts back in it (an explicit env var still wins). By name, not
     * values: the preset definition stays authoritative — editing it
     * changes what the next launch applies. Deliberately not
     * config_persist_selection, which is the exit-the-stance commit. */
    if (config_persist_state("preset", name) != 0) {
        static int warned;
        if (!warned) {
            warned = 1;
            ui_note("couldn't save to state.json — this preset applies to this session only");
        }
    }

    /* This is the ownership-transfer point: validation above keeps failure
     * theoretical, but preserve the old provider and override tier if it
     * still occurs. */
    if (agent_apply_settings(st, newp) != 0) {
        newp->destroy(newp);
        config_override_restore(ov);
        st->r->disp.trail = 1;
        goto out;
    }
    config_override_state_free(ov); /* committing — keep the applied overrides */

out:
    free(picked);
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* ---------- /config ---------- */

static int setting_is_bool(const struct config_setting *s)
{
    return s->choices && strcmp(s->choices, CONFIG_CHOICES_BOOL) == 0;
}

/* Tri-state boolean: on/off plus "auto", where auto (and unset) defers to the
 * consumer's own default — one it resolves itself (a provider preset), so
 * /config can't compute it and shows "auto" rather than a concrete on/off. */
static int setting_is_tristate(const struct config_setting *s)
{
    return s->choices && strcmp(s->choices, CONFIG_CHOICES_TRISTATE) == 0;
}

/* Display-safe effective value: secrets are redacted and booleans normalized.
 * config_str applies the registry's empty policy, so the shown value matches
 * what the setting actually reads (config_source mirrors the same rule).
 * Borrowed until the next override write. */
static const char *setting_display_value(const struct config_setting *s)
{
    const char *v = config_str(s->key);
    if (s->secret)
        return (v && *v) ? "set" : "unset";
    if (setting_is_bool(s))
        return config_bool(s->key) ? "on" : "off";
    if (setting_is_tristate(s)) {
        /* Unset or an explicit "auto" both mean "provider decides"; a valid
         * on/off spelling normalizes, an invalid value shows raw (+ marker). */
        if (!v || strcasecmp(v, "auto") == 0)
            return "auto";
        if (!config_value_valid(s, v))
            return v;
        return config_bool_or(s->key, 0) ? "on" : "off";
    }
    if (!v)
        return "unset";
    /* Only keep_empty settings resolve to a literal "" (config_str skips an
     * empty tier otherwise); that empty is meaningful — e.g. system_prompt ""
     * sends no system message where unset uses the built-in — so mark it. */
    if (!*v)
        return "(empty)";
    return v;
}

/* Whether the resolved value is present but fails validation — a bad env/
 * config entry the getter silently ignores in favor of the default. Free-form
 * (CFG_STRING without choices) always validates, so this fires for enums,
 * bools, and bounded numerics: a bad spelling or out-of-range value. */
static int setting_value_invalid(const struct config_setting *s)
{
    const char *v = config_str(s->key);
    return v && *v && !s->secret && !config_value_valid(s, v);
}

/* Print `key = value (source)` using the effective post-change value, marking
 * a configured value the getter rejects so it doesn't look like it applies. */
static void setting_note_current(struct agent_state *st, const struct config_setting *s)
{
    ui_note("%s = %s (%s%s)", s->key, setting_display_value(s), config_source(s->key),
            setting_value_invalid(s) ? ", invalid" : "");
    st->r->disp.trail = 1;
}

/* Dedicated command for settings that are runtime-changeable outside
 * /config. Kept here so slash-command names stay out of the config layer. */
static const char *setting_runtime_command(const char *key)
{
    static const struct {
        const char *key;
        const char *cmd;
    } cmds[] = {
        {"provider", "/provider"},
        {"model", "/model"},
        {"effort", "/effort"},
        {"preset", "/preset"},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        if (strcmp(cmds[i].key, key) == 0)
            return cmds[i].cmd;
    return NULL;
}

/* Show a read-only setting and where it can be changed. */
static void setting_note_readonly(struct agent_state *st, const struct config_setting *s)
{
    setting_note_current(st, s);
    const char *cmd = setting_runtime_command(s->key);
    if (cmd)
        ui_note("  change it with %s", cmd);
    else
        ui_note("  read-only at runtime — set %s or config.json and restart to change", s->env);
}

/* Set or clear the override, then refresh display-cached settings. */
static void setting_commit(struct agent_state *st, const struct config_setting *s, const char *val)
{
    config_set_override(s->key, val);
    agent_display_refresh(st);
    setting_note_current(st, s);
}

static size_t split_choices(const char *choices, char ***out)
{
    size_t n = 0, cap = 0;
    char **arr = NULL;
    const char *p = choices;
    for (;;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            arr = xrealloc(arr, cap * sizeof(*arr));
        }
        char *seg = xmalloc(len + 1);
        memcpy(seg, p, len);
        seg[len] = '\0';
        arr[n++] = seg;
        if (!bar)
            break;
        p = bar + 1;
    }
    *out = arr;
    return n;
}

/* Pick an enumerated value; "default" clears the override so lower tiers
 * resolve again. */
static void setting_pick_choice(struct agent_state *st, const struct config_setting *s)
{
    char **vals = NULL;
    size_t n = split_choices(s->choices, &vals);

    struct picker_item *items = xcalloc((n + 1), sizeof(*items));
    items[0].label = "default";
    items[0].detail = "clear the override — env/config resolves again";
    items[0].dim = 0;
    items[0].current = 0;
    const char *cur = setting_display_value(s);
    size_t initial = 0;
    for (size_t i = 0; i < n; i++) {
        items[i + 1].label = vals[i];
        items[i + 1].detail = NULL;
        items[i + 1].dim = 0;
        items[i + 1].current = strcasecmp(vals[i], cur) == 0;
        if (items[i + 1].current)
            initial = i + 1;
    }
    char *title = xasprintf("%s — %s", s->key, s->desc);
    struct picker_opts opts = {.title = title, .items = items, .n = n + 1, .initial = initial};
    long sel = picker_run(&opts);
    free(title);
    free(items);

    if (sel >= 0)
        setting_commit(st, s, sel == 0 ? NULL : vals[sel - 1]);
    for (size_t i = 0; i < n; i++)
        free(vals[i]);
    free(vals);
}

/* Seed the regular editor with a command for a free-form value. When the
 * current value fails validation (a bad env/config entry), seed the registry
 * default instead so pressing Enter commits something the getter accepts
 * rather than re-submitting the rejected value. */
static void setting_seed_prompt(struct agent_state *st, const struct config_setting *s)
{
    const char *v = config_str(s->key);
    if (v && *v && !config_value_valid(s, v))
        v = config_default(s->key);
    free(st->pending_preseed);
    st->pending_preseed =
        (v && *v) ? xasprintf("/config %s %s", s->key, v) : xasprintf("/config %s ", s->key);
}

static void config_typed(struct agent_state *st, const char *arg)
{
    const char *p = arg;
    while (*p && !isspace((unsigned char)*p))
        p++;
    char key[64];
    size_t klen = (size_t)(p - arg);
    if (klen >= sizeof(key)) {
        ui_error("unknown setting '%.*s'", (int)klen, arg);
        st->r->disp.trail = 1;
        return;
    }
    memcpy(key, arg, klen);
    key[klen] = '\0';
    while (*p && isspace((unsigned char)*p))
        p++;
    const char *val = *p ? p : NULL;

    const struct config_setting *s = config_setting_find(key);
    if (!s) {
        ui_error("unknown setting '%s' — /config lists them", key);
        st->r->disp.trail = 1;
        return;
    }
    if (!val) {
        if (s->runtime)
            setting_note_current(st, s);
        else
            setting_note_readonly(st, s);
        return;
    }
    if (!s->runtime) {
        const char *cmd = setting_runtime_command(key);
        if (cmd)
            ui_error("'%s' can't be changed from /config — use %s", key, cmd);
        else
            ui_error("'%s' can't be changed at runtime — set %s or config.json and restart", key,
                     s->env);
        st->r->disp.trail = 1;
        return;
    }
    if (strcmp(val, "default") == 0) {
        setting_commit(st, s, NULL);
        return;
    }
    if (!config_value_valid(s, val)) {
        char hint[64];
        config_value_hint(s, hint, sizeof(hint));
        ui_error("invalid value '%s' for %s (expected: %s, or default)", val, key, hint);
        st->r->disp.trail = 1;
        return;
    }
    /* Store the canonical spelling so a case-sensitive consumer matches. */
    char *canon = config_value_canonical(s, val);
    setting_commit(st, s, canon ? canon : val);
    free(canon);
}

void select_config(struct agent_state *st, const char *arg)
{
    if (arg && *arg) {
        config_typed(st, arg);
        return;
    }

    /* Preserve registry grouping; dim rows are inspectable but read-only. */
    size_t n = 0;
    const struct config_setting *rows = config_settings(&n);

    struct picker_item *items = xcalloc(n, sizeof(*items));
    char **details = xmalloc(n * sizeof(*details));
    char **descs = xcalloc(n, sizeof(*descs)); /* owned augmented descriptions */
    for (size_t i = 0; i < n; i++) {
        details[i] =
            xasprintf("%s (%s%s)", setting_display_value(&rows[i]), config_source(rows[i].key),
                      setting_value_invalid(&rows[i]) ? ", invalid" : "");
        items[i].label = rows[i].key;
        items[i].detail = details[i];
        /* Surface the value grammar that isn't obvious from the prose: units
         * for a size/duration, and the range for a bounded integer. A plain
         * unbounded integer needs no hint. */
        int show_hint = rows[i].kind == CFG_SIZE || rows[i].kind == CFG_DURATION ||
                        (rows[i].kind == CFG_INT && (rows[i].min || rows[i].max));
        if (show_hint) {
            char hint[64];
            config_value_hint(&rows[i], hint, sizeof(hint));
            descs[i] = xasprintf("%s (%s)", rows[i].desc, hint);
            items[i].desc = descs[i];
        } else {
            items[i].desc = rows[i].desc;
        }
        items[i].dim = !rows[i].runtime;
        items[i].current = 0;
    }
    struct picker_opts opts = {.title = "configuration", .items = items, .n = n, .initial = 0};
    long sel = picker_run(&opts);
    free(items);
    for (size_t i = 0; i < n; i++) {
        free(details[i]);
        free(descs[i]);
    }
    free(details);
    free(descs);

    if (sel >= 0) {
        const struct config_setting *s = &rows[sel];
        if (!s->runtime)
            setting_note_readonly(st, s);
        else if (s->choices)
            setting_pick_choice(st, s);
        else
            setting_seed_prompt(st, s);
    }
}
