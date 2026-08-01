/* SPDX-License-Identifier: MIT */
#include "model_meta.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "config.h"
#include "provider.h"
#include "util.h"
#include "system/bg_job.h"
#include "transport/http.h"

/* Live metadata for one provider's current model, plus the job fetching it.
 * Allocated on demand; hung off struct provider (see the `meta` field
 * there for why it lives per-provider). */
struct model_meta {
    struct bg_job *job;
    char *model;            /* what `info` describes; NULL = nothing held */
    struct model_info info; /* valid when model != NULL */
};

/* One lock for every provider's slot: readers copy fields out under it, the
 * single background writer publishes under it. Module-level rather than
 * per-provider to avoid initializing a mutex inside each adapter's calloc'd
 * struct; it guards no state of its own, and contention is nil (one probe
 * in flight at a time). */
static pthread_mutex_t meta_lock = PTHREAD_MUTEX_INITIALIZER;

static struct model_meta *meta_of(struct provider *p)
{
    if (!p->meta)
        p->meta = xcalloc(1, sizeof(*p->meta));
    return p->meta;
}

/* Drop the held snapshot. Caller holds the lock. */
static void meta_clear_locked(struct model_meta *m)
{
    free(m->model);
    m->model = NULL;
    model_info_clear(&m->info);
}

/* Settle the in-flight job so it cannot publish after this returns. Must be
 * called without the lock: the worker takes it to publish, so joining while
 * holding it would deadlock. */
static void meta_settle_job(struct model_meta *m)
{
    if (!m->job)
        return;
    bg_job_cancel(m->job);
    bg_job_join(m->job);
    m->job = NULL;
}

void model_meta_release(struct provider *p)
{
    if (!p || !p->meta)
        return;
    struct model_meta *m = p->meta;
    meta_settle_job(m);
    pthread_mutex_lock(&meta_lock);
    meta_clear_locked(m);
    pthread_mutex_unlock(&meta_lock);
    free(m);
    p->meta = NULL;
}

/* ---------------- the background probe ---------------- */

struct probe_job {
    struct model_meta *target; /* outlives the join — see model_meta_release */
    char *model;
    struct model_probe req;
};

static void probe_job_free(struct probe_job *job)
{
    model_probe_clear(&job->req);
    free(job->model);
    free(job);
}

static void probe_worker(struct bg_job *job, void *arg)
{
    struct probe_job *j = arg;
    /* Cancelled before the transfer started, where the tick that
     * short-circuits libcurl can't help yet. */
    if (bg_job_cancelled(job)) {
        probe_job_free(j);
        return;
    }

    char *body = NULL;
    int rc = http_get(j->req.url, (const char *const *)j->req.headers, j->req.timeout_s, 0,
                      bg_job_tick, job, &body, NULL);

    if (rc == 0 && body && !bg_job_cancelled(job)) {
        struct model_info info;
        model_info_init(&info);
        info.id = xstrdup(j->model);
        j->req.parse(body, j->model, &info);
        pthread_mutex_lock(&meta_lock);
        /* Publish only over the model this job was started for: a cancel
         * racing the parse must not overwrite a newer selection's answer
         * with this older one. */
        if (!j->target->model || strcmp(j->target->model, j->model) == 0) {
            meta_clear_locked(j->target);
            j->target->model = xstrdup(j->model);
            j->target->info = info;
            memset(&info, 0, sizeof(info)); /* ownership moved */
        }
        pthread_mutex_unlock(&meta_lock);
        model_info_clear(&info); /* no-op when it was adopted */
    }
    free(body);
    probe_job_free(j);
}

void model_meta_refresh(struct provider *p, const char *model)
{
    /* Nothing to fetch and nothing held: don't allocate a slot a backend
     * that can't describe its models would never fill. */
    if (!p || (!p->probe_model && !p->meta))
        return;
    struct model_meta *m = meta_of(p);

    /* Already describing this model — the picker handed its entry over on
     * the way past, from the same document a probe would fetch. Skipping
     * the round-trip is why a switch through /model costs one fetch, not
     * two. */
    pthread_mutex_lock(&meta_lock);
    int held = m->model && model && strcmp(m->model, model) == 0;
    pthread_mutex_unlock(&meta_lock);
    if (held)
        return;

    meta_settle_job(m);
    /* The held snapshot described the previous selection; keeping it would
     * answer the next question with the wrong model's numbers. */
    pthread_mutex_lock(&meta_lock);
    meta_clear_locked(m);
    pthread_mutex_unlock(&meta_lock);

    if (!p->probe_model || !model || !*model)
        return;
    struct model_probe req;
    memset(&req, 0, sizeof(req));
    if (p->probe_model(p, model, &req) != 0 || !req.url || !req.parse) {
        model_probe_clear(&req);
        return;
    }
    struct probe_job *j = xcalloc(1, sizeof(*j));
    j->target = m;
    j->model = xstrdup(model);
    j->req = req;
    m->job = bg_job_spawn(probe_worker, j);
    if (!m->job)
        probe_job_free(j); /* worker never ran, so nothing will free it */
}

void model_meta_settle(struct provider *p)
{
    if (!p || !p->meta || !p->meta->job)
        return;
    /* Joined, not cancelled: the point is to have the answer. */
    bg_job_join(p->meta->job);
    p->meta->job = NULL;
}

/* Does `src` state any fact a probe would otherwise go and fetch? */
static int describes_anything(const struct model_info *src)
{
    return src->context > 0 || src->max_output > 0 || src->image_input != PROVIDER_CAP_UNKNOWN ||
           src->tools != PROVIDER_CAP_UNKNOWN || src->efforts.known || src->cost_input >= 0 ||
           src->cost_output >= 0 || src->cost_cache_read >= 0 || src->cost_cache_write >= 0 ||
           src->cost_cache_write_1h >= 0 || src->n_tiers > 0;
}

void model_meta_remember(struct provider *p, const struct model_info *src)
{
    if (!p || !src || !src->id || !*src->id)
        return;
    /* A row that is nothing but an id describes no model — llama.cpp's
     * /v1/models, whose window and vision live in /props. Adopting it would
     * still count as a snapshot naming that model, which is what
     * model_meta_refresh skips the fetch on, so the answer would never
     * arrive. Return before touching the in-flight job, too: it may be
     * exactly that fetch. */
    if (!describes_anything(src))
        return;
    struct model_meta *m = meta_of(p);
    /* Same document the probe would fetch, so settle it rather than race. */
    meta_settle_job(m);
    pthread_mutex_lock(&meta_lock);
    meta_clear_locked(m);
    m->model = xstrdup(src->id);
    model_info_copy(&m->info, src);
    pthread_mutex_unlock(&meta_lock);
}

int model_meta_snapshot(const struct provider *p, struct model_info *out)
{
    model_info_init(out);
    if (!p || !p->meta)
        return 0;
    pthread_mutex_lock(&meta_lock);
    int held = p->meta->model != NULL;
    if (held)
        model_info_copy(out, &p->meta->info);
    pthread_mutex_unlock(&meta_lock);
    return held;
}

/* ---------------- resolution ---------------- */

/* Copy the live snapshot for `model` under the lock, or return 0. */
static int live_copy(const struct provider *p, const char *model, struct model_info *out)
{
    if (!p || !p->meta || !model || !*model)
        return 0;
    pthread_mutex_lock(&meta_lock);
    int ok = p->meta->model && strcmp(p->meta->model, model) == 0;
    if (ok)
        model_info_copy(out, &p->meta->info);
    pthread_mutex_unlock(&meta_lock);
    return ok;
}

static void load_catalog_entry(const struct provider *p, const char *model,
                               struct catalog_entry *out)
{
    catalog_entry_init(out);
    if (p && p->catalog_id && *p->catalog_id && model && *model)
        catalog_lookup(p->catalog_id, model, out);
}

void model_meta_merge(const struct model_info *reported, const struct catalog_entry *catalog,
                      struct model_info *out)
{
    model_info_init(out);
    if (reported) {
        struct model_info reported_view = *reported;
        reported_view.id = NULL; /* The merged view describes a model; it is not a model row. */
        reported_view.description = NULL;
        *out = reported_view;
    }
    if (!catalog)
        return;
    int backend_has_rates = reported && (reported->cost_input >= 0 || reported->cost_output >= 0);
    if (out->context <= 0)
        out->context = catalog->context_window;
    if (out->max_output <= 0)
        out->max_output = catalog->max_output;
    if (out->image_input == PROVIDER_CAP_UNKNOWN && catalog->image_input != CATALOG_SUPPORT_UNKNOWN)
        out->image_input =
            catalog->image_input == CATALOG_SUPPORT_YES ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    if (out->cost_input < 0)
        out->cost_input = catalog->cost_input;
    if (out->cost_output < 0)
        out->cost_output = catalog->cost_output;
    if (out->cost_cache_read < 0)
        out->cost_cache_read = catalog->cost_cache_read;
    if (out->cost_cache_write < 0)
        out->cost_cache_write = catalog->cost_cache_write;
    if (out->cost_cache_write_1h < 0)
        out->cost_cache_write_1h = catalog->cost_cache_write_1h;
    /* Tiers move whole, and only onto a report with no rates of its own:
     * the backend's base rates under the snapshot's thresholds would bill
     * a request at rates that never coexisted. A backend quoting rates but
     * no tiers is taken at its word — flat. */
    if (out->n_tiers == 0 && !backend_has_rates) {
        memcpy(out->tiers, catalog->tiers, sizeof(out->tiers));
        out->n_tiers = catalog->n_tiers;
    }
    if (!out->efforts.known)
        out->efforts = catalog->efforts;
}

static void resolve_model_info(const struct provider *p, const char *model, struct model_info *out)
{
    struct model_info live;
    int has_live_metadata = live_copy(p, model, &live);
    struct catalog_entry catalog;
    load_catalog_entry(p, model, &catalog);
    model_meta_merge(has_live_metadata ? &live : NULL, &catalog, out);
    if (has_live_metadata)
        model_info_clear(&live);
}

long model_meta_context(const struct provider *p, const char *model)
{
    long configured_context = config_size("context_limit");
    if (configured_context > 0)
        return configured_context;
    struct model_info info;
    resolve_model_info(p, model, &info);
    return info.context;
}

long model_meta_max_output(const struct provider *p, const char *model)
{
    struct model_info info;
    resolve_model_info(p, model, &info);
    return info.max_output;
}

int model_meta_rates(const struct provider *p, const char *model, struct catalog_entry *out)
{
    struct model_info info;
    resolve_model_info(p, model, &info);
    catalog_entry_init(out);
    out->cost_input = info.cost_input;
    out->cost_output = info.cost_output;
    out->cost_cache_read = info.cost_cache_read;
    out->cost_cache_write = info.cost_cache_write;
    out->cost_cache_write_1h = info.cost_cache_write_1h;
    memcpy(out->tiers, info.tiers, sizeof(out->tiers));
    out->n_tiers = info.n_tiers;
    out->tiers_declared = 1;
    return info.cost_input >= 0 && info.cost_output >= 0;
}

int model_meta_image_input(const struct provider *p, const char *model)
{
    /* "auto" falls through to detection; a real on/off pins the answer.
     * Tested by string — the bool accessors can't express "not a bool". */
    const char *configured = config_str("image_input");
    if (configured && *configured && strcmp(configured, "auto") != 0)
        return config_bool("image_input");
    struct model_info info;
    resolve_model_info(p, model, &info);
    enum provider_cap capability = info.image_input;
    if (capability == PROVIDER_CAP_YES)
        return 1;
    if (capability == PROVIDER_CAP_NO)
        return 0;
    return -1;
}

void model_meta_efforts(const struct provider *p, const char *model, struct effort_set *out)
{
    memset(out, 0, sizeof(*out));
    out->known = 1;

    /* The provider's static ladder is the vocabulary hax knows how to send
     * on this backend, so an empty one settles the question before any
     * metadata is consulted: a provider that never sends an effort field
     * must not grow a menu because a catalog happens to describe its
     * model. It also supplies the presentation order below. */
    const char *const *ladder = NULL;
    struct provider *mutable_provider = (struct provider *)p;
    size_t ladder_count = (p && p->list_efforts) ? p->list_efforts(mutable_provider, &ladder) : 0;
    if (ladder_count == 0)
        return;

    struct effort_set reported = {0};
    struct model_info live;
    if (live_copy(p, model, &live)) {
        reported = live.efforts;
        model_info_clear(&live);
    }
    /* Which tier answered decides whether it may widen the ladder below. */
    int from_backend = reported.known;
    if (!reported.known) {
        struct catalog_entry catalog;
        load_catalog_entry(p, model, &catalog);
        reported = catalog.efforts;
    }

    if (!reported.known) {
        for (size_t i = 0; i < ladder_count; i++)
            effort_set_add(out, ladder[i]);
        return;
    }
    if (reported.n == 0)
        return; /* known to take no levels — the empty answer is the answer */

    for (size_t i = 0; i < ladder_count; i++)
        if (effort_set_has(&reported, ladder[i]))
            effort_set_add(out, ladder[i]);
    /* Only a backend may name a level the ladder doesn't: it is describing
     * the model it will itself serve. The catalog is keyed by a shared id
     * (codex borrows "openai" for want of an entry of its own), so its
     * vocabulary is some other API's — "minimal" is an OpenAI level the
     * codex backend answers 400 for — and it narrows only. */
    if (from_backend)
        for (size_t i = 0; i < reported.n; i++)
            effort_set_add(out, reported.v[i]);
}

/* Canonical ordering for clamping, separate from any provider's ladder: the
 * level being placed is by definition absent from the set being searched,
 * so only a shared scale can say which neighbor is nearer. An unrecognized
 * name has no rank and declines to clamp rather than guessing. */
static const char *const EFFORT_RANK[] = {"none", "minimal", "low", "medium",
                                          "high", "xhigh",   "max"};

static int effort_rank(const char *level)
{
    if (!level)
        return -1;
    for (size_t i = 0; i < sizeof(EFFORT_RANK) / sizeof(EFFORT_RANK[0]); i++)
        if (strcmp(EFFORT_RANK[i], level) == 0)
            return (int)i;
    return -1;
}

const char *effort_clamp(const struct effort_set *s, const char *want)
{
    if (!s->known || s->n == 0 || !want || !*want)
        return NULL;
    if (effort_set_has(s, want))
        return want;
    int target = effort_rank(want);
    if (target < 0)
        return NULL;

    const char *below = NULL, *above = NULL;
    int below_rank = -1, above_rank = 0;
    for (size_t i = 0; i < s->n; i++) {
        int rank = effort_rank(s->v[i]);
        if (rank < 0)
            continue;
        if (rank <= target && rank > below_rank) {
            below_rank = rank;
            below = s->v[i];
        }
        if (rank > target && (!above || rank < above_rank)) {
            above_rank = rank;
            above = s->v[i];
        }
    }
    return below ? below : above;
}
