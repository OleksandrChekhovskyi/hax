/* SPDX-License-Identifier: MIT */
#include "model_meta.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "config.h"
#include "effort.h"
#include "provider.h"
#include "util.h"
#include "system/bg_job.h"
#include "transport/http.h"

struct model_meta {
    struct bg_job *probe_job;
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
        /* A cancelled probe must not overwrite a newer selection after parsing. */
        if (!task->target->reported.id || strcmp(task->target->reported.id, task->model_id) == 0) {
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
    pthread_mutex_lock(&report_lock);
    int already_reported = meta->reported.id && model && strcmp(meta->reported.id, model) == 0;
    pthread_mutex_unlock(&report_lock);
    if (already_reported)
        return;

    cancel_probe(meta);
    pthread_mutex_lock(&report_lock);
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
    meta->probe_job = bg_job_spawn(probe_worker, task);
    if (!meta->probe_job)
        probe_task_free(task);
}

void model_meta_wait(struct provider *provider)
{
    if (!provider || !provider->meta || !provider->meta->probe_job)
        return;
    bg_job_join(provider->meta->probe_job);
    provider->meta->probe_job = NULL;
}

static int model_info_has_details(const struct model_info *info)
{
    return info->context > 0 || info->max_output > 0 || info->image_input != PROVIDER_CAP_UNKNOWN ||
           info->tools != PROVIDER_CAP_UNKNOWN || info->efforts.known || info->cost_input >= 0 ||
           info->cost_output >= 0 || info->cost_cache_read >= 0 || info->cost_cache_write >= 0 ||
           info->cost_cache_write_1h >= 0 || info->n_tiers > 0;
}

void model_meta_store(struct provider *provider, const struct model_info *info)
{
    if (!provider || !info || !info->id || !*info->id || !model_info_has_details(info))
        return;

    struct model_meta *meta = get_or_create_meta(provider);
    cancel_probe(meta);
    pthread_mutex_lock(&report_lock);
    clear_report_locked(meta);
    model_info_copy(&meta->reported, info);
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
    if (provider && provider->catalog_id && *provider->catalog_id && model && *model)
        catalog_lookup(provider->catalog_id, model, out);
}

static int report_has_base_rates(const struct model_info *report)
{
    return report && (report->cost_input >= 0 || report->cost_output >= 0);
}

void model_meta_merge(const struct model_info *reported, const struct catalog_entry *catalog,
                      struct model_info *out)
{
    model_info_init(out);
    if (reported) {
        *out = *reported;
        out->id = NULL;
        out->description = NULL;
    }
    if (!catalog)
        return;

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

    /* Catalog tiers cannot be combined with base rates reported by a different billing source. */
    if (out->n_tiers == 0 && !report_has_base_rates(reported)) {
        memcpy(out->tiers, catalog->tiers, sizeof(out->tiers));
        out->n_tiers = catalog->n_tiers;
    }
    if (!out->efforts.known)
        out->efforts = catalog->efforts;
}

static void resolve_model_info(const struct provider *provider, const char *model,
                               struct model_info *out)
{
    struct model_info reported;
    int has_report = copy_report(provider, model, &reported);
    struct catalog_entry catalog;
    load_catalog_entry(provider, model, &catalog);
    model_meta_merge(has_report ? &reported : NULL, &catalog, out);
    model_info_clear(&reported);
}

long model_meta_context(const struct provider *provider, const char *model)
{
    long configured = config_size("context_limit");
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

    struct effort_set accepted = {0};
    struct model_info reported;
    if (copy_report(provider, model, &reported)) {
        accepted = reported.efforts;
        model_info_clear(&reported);
    }
    int reported_by_provider = accepted.known;
    if (!accepted.known) {
        struct catalog_entry catalog;
        load_catalog_entry(provider, model, &catalog);
        accepted = catalog.efforts;
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

    /* Catalog metadata may narrow a provider's vocabulary; only the provider may extend it. */
    if (reported_by_provider)
        for (size_t i = 0; i < accepted.count; i++)
            effort_set_add(out, accepted.values[i]);
}
