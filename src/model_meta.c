/* SPDX-License-Identifier: MIT */
#include "model_meta.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "config.h"
#include "effort.h"
#include "provider.h"
#include "xalloc.h"
#include "system/bg_job.h"
#include "system/clock.h"
#include "transport/http.h"

struct model_meta {
    struct bg_job *probe_job;
    char *probe_model;     /* owned; model the active probe targets */
    long probe_started_ms; /* monotonic; anchors the shared model_meta_wait_ms budget */
    struct model_info reported;
};

/* Provider slots are foreground-owned; this lock protects reports shared with probe workers. */
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;

static struct model_meta *get_or_create_meta(struct provider *provider)
{
    if (!provider->meta)
        provider->meta = xcalloc(1, sizeof(*provider->meta));
    return provider->meta;
}

static void clear_report_locked(struct model_meta *meta)
{
    model_info_clear(&meta->reported);
}

/* Joining while holding report_lock would deadlock with a worker publishing its result. */
static void cancel_probe(struct model_meta *meta)
{
    if (!meta->probe_job)
        return;
    bg_job_cancel(meta->probe_job);
    bg_job_join(meta->probe_job);
    meta->probe_job = NULL;
    free(meta->probe_model);
    meta->probe_model = NULL;
}

void model_meta_release(struct provider *provider)
{
    if (!provider || !provider->meta)
        return;

    struct model_meta *meta = provider->meta;
    cancel_probe(meta);
    pthread_mutex_lock(&report_lock);
    clear_report_locked(meta);
    pthread_mutex_unlock(&report_lock);
    free(meta);
    provider->meta = NULL;
}

struct probe_task {
    struct model_meta *target; /* valid until the owning provider joins the probe */
    char *model_id;
    struct model_probe request;
};

static void probe_task_free(struct probe_task *task)
{
    model_probe_clear(&task->request);
    free(task->model_id);
    free(task);
}

/* Fill fields `report` leaves unknown from `retained`. Newly parsed values win. */
static void report_fill_unknown(struct model_info *report, const struct model_info *retained)
{
    if (report->context <= 0)
        report->context = retained->context;
    if (report->max_context <= 0)
        report->max_context = retained->max_context;
    if (report->max_output <= 0)
        report->max_output = retained->max_output;
    if (report->image_input == PROVIDER_CAP_UNKNOWN)
        report->image_input = retained->image_input;
    if (report->tools == PROVIDER_CAP_UNKNOWN)
        report->tools = retained->tools;
    if (report->cost_input < 0)
        report->cost_input = retained->cost_input;
    if (report->cost_cache_read < 0)
        report->cost_cache_read = retained->cost_cache_read;
    if (report->cost_output < 0)
        report->cost_output = retained->cost_output;
    if (report->cost_cache_write < 0)
        report->cost_cache_write = retained->cost_cache_write;
    if (report->cost_cache_write_1h < 0)
        report->cost_cache_write_1h = retained->cost_cache_write_1h;
    if (report->n_tiers == 0) {
        memcpy(report->tiers, retained->tiers, sizeof(report->tiers));
        report->n_tiers = retained->n_tiers;
    }
    if (!report->efforts.known)
        report->efforts = retained->efforts;
    if (!report->description && retained->description)
        report->description = xstrdup(retained->description);
}

static void probe_worker(struct bg_job *job, void *arg)
{
    struct probe_task *task = arg;
    /* The HTTP tick cannot observe cancellation until the transfer starts. */
    if (bg_job_cancel_requested(job)) {
        probe_task_free(task);
        return;
    }

    char *body = NULL;
    int rc = http_get(task->request.url, (const char *const *)task->request.headers,
                      task->request.timeout_s, 0, bg_job_cancel_tick, job, &body, NULL);
    if (rc == 0 && body && !bg_job_cancel_requested(job)) {
        struct model_info report;
        model_info_init(&report);
        report.id = xstrdup(task->model_id);
        task->request.parse(body, task->model_id, &report);

        pthread_mutex_lock(&report_lock);
        /* A cancelled probe must not overwrite a newer selection after parsing. A retained
         * same-model report fills what the probe could not learn; an absent report is zeroed
         * state, not knowledge, and must not be merged from. */
        if (!task->target->reported.id || strcmp(task->target->reported.id, task->model_id) == 0) {
            if (task->target->reported.id)
                report_fill_unknown(&report, &task->target->reported);
            clear_report_locked(task->target);
            model_info_copy(&task->target->reported, &report);
        }
        pthread_mutex_unlock(&report_lock);
        model_info_clear(&report);
    }

    free(body);
    probe_task_free(task);
}

void model_meta_refresh(struct provider *provider, const char *model)
{
    if (!provider || (!provider->probe_model && !provider->meta))
        return;

    struct model_meta *meta = get_or_create_meta(provider);
    /* Reap a finished probe so it cannot pass for a live one below. */
    if (meta->probe_job && bg_job_wait_ms(meta->probe_job, 0))
        model_meta_wait(provider);

    pthread_mutex_lock(&report_lock);
    /* A model-list report may lack the context window; a probe can still learn it. */
    int report_complete = meta->reported.context > 0 || !provider->probe_model;
    int already_reported =
        meta->reported.id && model && strcmp(meta->reported.id, model) == 0 && report_complete;
    pthread_mutex_unlock(&report_lock);
    if (already_reported)
        return;

    /* A live probe for this model is already doing this refresh's work — and cancelling it would
     * abort a router warm-up mid-load. */
    if (meta->probe_job && meta->probe_model && model && strcmp(meta->probe_model, model) == 0)
        return;

    cancel_probe(meta);
    pthread_mutex_lock(&report_lock);
    /* Retain a same-model partial report so its fields survive a slow or failed probe. */
    int same_model = meta->reported.id && model && strcmp(meta->reported.id, model) == 0;
    if (!same_model)
        clear_report_locked(meta);
    pthread_mutex_unlock(&report_lock);

    if (!provider->probe_model || !model || !*model)
        return;

    struct model_probe request = {0};
    if (provider->probe_model(provider, model, &request) != 0 || !request.url || !request.parse) {
        model_probe_clear(&request);
        return;
    }

    struct probe_task *task = xcalloc(1, sizeof(*task));
    task->target = meta;
    task->model_id = xstrdup(model);
    task->request = request;
    meta->probe_started_ms = monotonic_ms();
    meta->probe_job = bg_job_spawn(probe_worker, task);
    if (meta->probe_job)
        meta->probe_model = xstrdup(model);
    else
        probe_task_free(task);
}

void model_meta_wait(struct provider *provider)
{
    if (!provider || !provider->meta || !provider->meta->probe_job)
        return;
    bg_job_join(provider->meta->probe_job);
    provider->meta->probe_job = NULL;
    free(provider->meta->probe_model);
    provider->meta->probe_model = NULL;
}

void model_meta_wait_ms(struct provider *provider, long timeout_ms)
{
    if (!provider || !provider->meta || !provider->meta->probe_job)
        return;
    /* The budget is anchored at probe start so stacked callers on one request path do not each
     * wait the full amount for a slow probe. */
    long remaining_ms = timeout_ms - (monotonic_ms() - provider->meta->probe_started_ms);
    if (bg_job_wait_ms(provider->meta->probe_job, remaining_ms > 0 ? remaining_ms : 0))
        model_meta_wait(provider);
}

static int model_info_has_details(const struct model_info *info)
{
    return info->context > 0 || info->max_context > 0 || info->max_output > 0 ||
           info->image_input != PROVIDER_CAP_UNKNOWN || info->tools != PROVIDER_CAP_UNKNOWN ||
           info->efforts.known || info->cost_input >= 0 || info->cost_output >= 0 ||
           info->cost_cache_read >= 0 || info->cost_cache_write >= 0 ||
           info->cost_cache_write_1h >= 0 || info->n_tiers > 0;
}

void model_meta_store(struct provider *provider, const struct model_info *info)
{
    if (!provider || !info || !info->id || !*info->id || !model_info_has_details(info))
        return;

    struct model_meta *meta = get_or_create_meta(provider);
    pthread_mutex_lock(&report_lock);
    int same_report = meta->reported.id && strcmp(meta->reported.id, info->id) == 0;
    pthread_mutex_unlock(&report_lock);
    /* A same-model store refines the report and leaves an active probe (a router warm-up, say)
     * running to merge its result later; a different model invalidates both. */
    int same_probe = meta->probe_model && strcmp(meta->probe_model, info->id) == 0;
    if (!same_report && !same_probe)
        cancel_probe(meta);

    struct model_info merged;
    model_info_copy(&merged, info);
    pthread_mutex_lock(&report_lock);
    /* Recheck under the lock: a kept probe may have published since the check above. An absent
     * or foreign report is zeroed state, not knowledge; merge only from a match. */
    if (meta->reported.id && strcmp(meta->reported.id, info->id) == 0)
        report_fill_unknown(&merged, &meta->reported);
    clear_report_locked(meta);
    meta->reported = merged; /* ownership moves; merged must not be cleared */
    pthread_mutex_unlock(&report_lock);
}

int model_meta_snapshot(const struct provider *provider, struct model_info *out)
{
    model_info_init(out);
    if (!provider || !provider->meta)
        return 0;

    pthread_mutex_lock(&report_lock);
    int has_report = provider->meta->reported.id != NULL;
    if (has_report)
        model_info_copy(out, &provider->meta->reported);
    pthread_mutex_unlock(&report_lock);
    return has_report;
}

static int copy_report(const struct provider *provider, const char *model, struct model_info *out)
{
    model_info_init(out);
    if (!provider || !provider->meta || !model || !*model)
        return 0;

    pthread_mutex_lock(&report_lock);
    int matches = provider->meta->reported.id && strcmp(provider->meta->reported.id, model) == 0;
    if (matches)
        model_info_copy(out, &provider->meta->reported);
    pthread_mutex_unlock(&report_lock);
    return matches;
}

static void load_catalog_entry(const struct provider *provider, const char *model,
                               struct catalog_entry *out)
{
    catalog_entry_init(out);
    if (provider && model && *model)
        catalog_lookup(provider_stable_id(provider), provider->catalog_id, model, out);
}

/* Returns 1 when configuration declares any metadata for `model`. */
static int load_config_entry(const struct provider *provider, const char *model,
                             struct catalog_entry *out)
{
    catalog_entry_init(out);
    if (!provider || !model || !*model)
        return 0;
    return catalog_lookup_config(provider_stable_id(provider), provider->catalog_id, model, out) ==
           0;
}

static int report_has_base_rates(const struct model_info *report)
{
    return report && (report->cost_input >= 0 || report->cost_output >= 0);
}

/* Apply one catalog-shaped layer: with `entry_wins`, known entry fields replace `out`'s; without
 * it they only fill unknowns. Tiers stay with the caller — their rule spans layers. */
static void merge_entry_fields(struct model_info *out, const struct catalog_entry *entry,
                               int entry_wins)
{
    if (entry->context_window > 0 && (entry_wins || out->context <= 0))
        out->context = entry->context_window;
    if (entry->max_output > 0 && (entry_wins || out->max_output <= 0))
        out->max_output = entry->max_output;
    if (entry->image_input != CATALOG_SUPPORT_UNKNOWN &&
        (entry_wins || out->image_input == PROVIDER_CAP_UNKNOWN))
        out->image_input =
            entry->image_input == CATALOG_SUPPORT_YES ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    if (entry->cost_input >= 0 && (entry_wins || out->cost_input < 0))
        out->cost_input = entry->cost_input;
    if (entry->cost_output >= 0 && (entry_wins || out->cost_output < 0))
        out->cost_output = entry->cost_output;
    if (entry->cost_cache_read >= 0 && (entry_wins || out->cost_cache_read < 0))
        out->cost_cache_read = entry->cost_cache_read;
    if (entry->cost_cache_write >= 0 && (entry_wins || out->cost_cache_write < 0))
        out->cost_cache_write = entry->cost_cache_write;
    if (entry->cost_cache_write_1h >= 0 && (entry_wins || out->cost_cache_write_1h < 0))
        out->cost_cache_write_1h = entry->cost_cache_write_1h;
    if (entry->efforts.known && (entry_wins || !out->efforts.known))
        out->efforts = entry->efforts;
}

void model_meta_merge(const struct catalog_entry *configured, const struct model_info *reported,
                      const struct catalog_entry *catalog, struct model_info *out)
{
    model_info_init(out);
    if (reported) {
        *out = *reported;
        out->id = NULL;
        out->description = NULL;
    }
    if (catalog) {
        merge_entry_fields(out, catalog, 0);
        /* Snapshot tiers cannot be combined with base rates reported by a different billing
         * source. */
        if (out->n_tiers == 0 && !report_has_base_rates(reported)) {
            memcpy(out->tiers, catalog->tiers, sizeof(out->tiers));
            out->n_tiers = catalog->n_tiers;
        }
    }
    if (configured) {
        merge_entry_fields(out, configured, 1);
        /* Configured tiers were written against the user's own rate choices and always apply. */
        if (configured->tiers_declared) {
            memcpy(out->tiers, configured->tiers, sizeof(out->tiers));
            out->n_tiers = configured->n_tiers;
        }
    }
}

static void resolve_model_info(const struct provider *provider, const char *model,
                               struct model_info *out)
{
    struct catalog_entry configured;
    int has_config = load_config_entry(provider, model, &configured);
    struct model_info reported;
    int has_report = copy_report(provider, model, &reported);
    struct catalog_entry catalog;
    load_catalog_entry(provider, model, &catalog);
    model_meta_merge(has_config ? &configured : NULL, has_report ? &reported : NULL, &catalog, out);
    model_info_clear(&reported);
}

long model_meta_context(const struct provider *provider, const char *model)
{
    long configured = config_tokens("context_limit");
    if (configured > 0)
        return configured;

    struct model_info info;
    resolve_model_info(provider, model, &info);
    return info.context;
}

long model_meta_max_output(const struct provider *provider, const char *model)
{
    struct model_info info;
    resolve_model_info(provider, model, &info);
    return info.max_output;
}

int model_meta_rates(const struct provider *provider, const char *model, struct catalog_entry *out)
{
    struct model_info info;
    resolve_model_info(provider, model, &info);
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

int model_meta_image_input(const struct provider *provider, const char *model)
{
    const char *configured = config_str("image_input");
    if (configured && *configured && strcmp(configured, "auto") != 0)
        return config_bool("image_input");

    struct model_info info;
    resolve_model_info(provider, model, &info);
    if (info.image_input == PROVIDER_CAP_YES)
        return 1;
    if (info.image_input == PROVIDER_CAP_NO)
        return 0;
    return -1;
}

void model_meta_efforts(const struct provider *provider, const char *model, struct effort_set *out)
{
    memset(out, 0, sizeof(*out));
    out->known = 1;

    const char *const *provider_levels = NULL;
    struct provider *mutable_provider = (struct provider *)provider;
    size_t provider_level_count = (provider && provider->list_efforts)
                                      ? provider->list_efforts(mutable_provider, &provider_levels)
                                      : 0;
    /* Metadata cannot enable effort values on a provider that has no way to send them. */
    if (provider_level_count == 0)
        return;

    struct catalog_entry configured;
    int authoritative = load_config_entry(provider, model, &configured) && configured.efforts.known;
    struct effort_set accepted = {0};
    if (authoritative) {
        accepted = configured.efforts;
    } else {
        struct model_info reported;
        if (copy_report(provider, model, &reported)) {
            accepted = reported.efforts;
            model_info_clear(&reported);
        }
        authoritative = accepted.known;
        if (!accepted.known) {
            struct catalog_entry catalog;
            load_catalog_entry(provider, model, &catalog);
            accepted = catalog.efforts;
        }
    }

    if (!accepted.known) {
        for (size_t i = 0; i < provider_level_count; i++)
            effort_set_add(out, provider_levels[i]);
        return;
    }
    if (accepted.count == 0)
        return;

    for (size_t i = 0; i < provider_level_count; i++)
        if (effort_set_has(&accepted, provider_levels[i]))
            effort_set_add(out, provider_levels[i]);

    /* Snapshot metadata may narrow a provider's vocabulary; only the provider's own report or
     * explicit configuration may extend it. */
    if (authoritative)
        for (size_t i = 0; i < accepted.count; i++)
            effort_set_add(out, accepted.values[i]);
}
