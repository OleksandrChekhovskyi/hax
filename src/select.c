/* SPDX-License-Identifier: MIT */
#include "select.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"
#include "agent.h"
#include "agent_core.h"
#include "busy.h"
#include "catalog.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "providers/registry.h"
#include "render/render_ctx.h"
#include "system/bg_job.h"
#include "terminal/picker.h"
#include "terminal/theme.h"
#include "terminal/ui.h"
#include "transport/http.h"

/* ---------- parallel availability probe ---------- */

struct availability_result {
    int available;
    const char *reason; /* static provider reason */
};

struct availability_job_ctx {
    struct provider_availability probe;
    struct availability_result *result;
};

static void prepare_availability(const struct provider_factory *factory,
                                 struct provider_availability *probe)
{
    memset(probe, 0, sizeof(*probe));
    probe->available = 1; /* no hook means immediately available */
    if (factory->prepare_availability)
        factory->prepare_availability(factory->name, probe);
}

/* The worker receives a complete owned request. In particular, provider
 * hooks and config resolution have already run on the foreground thread. */
static void availability_worker(struct bg_job *job, void *arg)
{
    (void)job;
    struct availability_job_ctx *ctx = arg;
    char *body = NULL;
    int available = http_get(ctx->probe.url, (const char *const *)ctx->probe.headers,
                             ctx->probe.timeout_s, 0, NULL, NULL, &body, NULL) == 0;
    free(body);
    ctx->result->available = available;
    ctx->result->reason =
        available ? NULL : (ctx->probe.reason ? ctx->probe.reason : "unavailable");
    provider_availability_clear(&ctx->probe);
    free(ctx);
}

static int factory_available(const struct provider_factory *factory, const char **reason)
{
    struct provider_availability probe;
    prepare_availability(factory, &probe);
    int available = probe.available;
    if (probe.url) {
        char *body = NULL;
        available = http_get(probe.url, (const char *const *)probe.headers, probe.timeout_s, 0,
                             NULL, NULL, &body, NULL) == 0;
        free(body);
    }
    if (reason)
        *reason = available ? NULL : (probe.reason ? probe.reason : "unavailable");
    provider_availability_clear(&probe);
    return available;
}

/* Resolve provider config on the foreground; only prepared network probes run concurrently. */
static void probe_availability(const struct provider_factory *const *factories, size_t count,
                               struct availability_result *results)
{
    struct bg_job **jobs = xcalloc(count, sizeof(*jobs));
    for (size_t i = 0; i < count; i++) {
        results[i].available = 1; /* default if a network worker cannot be spawned */
        results[i].reason = NULL;
        struct availability_job_ctx *ctx = xcalloc(1, sizeof(*ctx));
        prepare_availability(factories[i], &ctx->probe);
        if (!ctx->probe.url) {
            results[i].available = ctx->probe.available;
            results[i].reason = results[i].available
                                    ? NULL
                                    : (ctx->probe.reason ? ctx->probe.reason : "unavailable");
            provider_availability_clear(&ctx->probe);
            free(ctx);
            continue;
        }
        ctx->result = &results[i];
        jobs[i] = bg_job_spawn(availability_worker, ctx);
        if (!jobs[i]) {
            provider_availability_clear(&ctx->probe);
            free(ctx);
        }
    }
    for (size_t i = 0; i < count; i++)
        bg_job_join(jobs[i]); /* NULL is a no-op */
    free(jobs);
}

/* Try providers in autoselect priority order. The inferred choice is run-scoped; an explicit
 * selector later promotes it to persisted state. */
struct provider *provider_autoselect(void)
{
    /* Avoid parallel probe setup when the inexpensive default succeeds. */
    const struct provider_factory *default_factory = provider_default();
    if (default_factory && factory_available(default_factory, NULL)) {
        struct provider *provider = default_factory->new(default_factory->name);
        if (provider) {
            /* Expose the inferred provider to later selectors without persisting it. */
            config_set_override("provider", default_factory->name);
            return provider;
        }
    }

    /* Probe the remaining providers concurrently, then construct in priority order. */
    size_t factory_count = 0;
    const struct provider_factory *const *factories = provider_all(&factory_count);
    struct availability_result *availability = xcalloc(factory_count, sizeof(*availability));
    probe_availability(factories, factory_count, availability);

    struct provider *provider = NULL;
    for (size_t i = 0; i < factory_count && !provider; i++) {
        if (factories[i] == default_factory || !availability[i].available)
            continue;
        /* Availability can change between probe and construction; continue on failure. */
        provider = factories[i]->new(factories[i]->name);
        if (provider)
            config_set_override("provider", factories[i]->name);
    }
    free(availability);
    return provider;
}

/* ---------- model / effort pick steps ---------- */

static int compare_string_pointers(const void *left, const void *right)
{
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static int compare_model_info(const void *left, const void *right)
{
    const struct model_info *left_model = left;
    const struct model_info *right_model = right;
    return strcmp(left_model->id, right_model->id);
}

/* ---------- /model picker gutter ---------- */

/* Keep one decimal for millions, trimming a zero fraction. */
static void format_token_count(char *output, size_t output_size, long tokens)
{
    if (tokens >= 1000000) {
        snprintf(output, output_size, "%.1fM", (double)tokens / 1000000.0);
        char *zero_fraction = strstr(output, ".0M");
        if (zero_fraction)
            memmove(zero_fraction, zero_fraction + 2, strlen(zero_fraction + 2) + 1);
    } else if (tokens >= 1000) {
        snprintf(output, output_size, "%ldk", tokens / 1000);
    } else {
        snprintf(output, output_size, "%ld", tokens);
    }
}

static void append_segment(struct buf *buffer, const char *text)
{
    if (buffer->len)
        buf_append_str(buffer, " · ");
    buf_append_str(buffer, text);
}

/* Split registry choices into an owned array; the caller frees each string and the array. */
static size_t split_choices(const char *choices, char ***values_out)
{
    size_t count = 0;
    size_t capacity = 0;
    char **values = NULL;
    const char *start = choices;
    for (;;) {
        const char *separator = strchr(start, '|');
        size_t length = separator ? (size_t)(separator - start) : strlen(start);
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            values = xrealloc(values, capacity * sizeof(*values));
        }
        char *value = xmalloc(length + 1);
        memcpy(value, start, length);
        value[length] = '\0';
        values[count++] = value;
        if (!separator)
            break;
        start = separator + 1;
    }
    *values_out = values;
    return count;
}

char *model_desc_line(const struct model_info *model, const struct catalog_entry *catalog)
{
    /* Backend metadata is fresher and more specific, so it overrides catalog fields. */
    struct model_info merged;
    model_meta_merge(model, catalog, &merged);
    long context_tokens = merged.context;
    int image_input = merged.image_input;
    double input_cost = merged.cost_input;
    double output_cost = merged.cost_output;
    double cached_input_cost = merged.cost_cache_read;

    struct buf description;
    buf_init(&description);
    if (context_tokens > 0) {
        char token_count[32];
        char segment[48];
        format_token_count(token_count, sizeof(token_count), context_tokens);
        snprintf(segment, sizeof(segment), "%s context", token_count);
        append_segment(&description, segment);
    }
    /* Image support is usually present, so describe only its absence. Tool support controls row
     * availability rather than model description. */
    if (image_input == PROVIDER_CAP_NO)
        append_segment(&description, "no images");
    if (input_cost >= 0 && output_cost >= 0) {
        char segment[96];
        if (input_cost == 0 && output_cost == 0) {
            snprintf(segment, sizeof(segment), "free");
        } else if (cached_input_cost >= 0) {
            /* Cache discounts vary by backend and can dominate long conversations. */
            snprintf(segment, sizeof(segment), "$%.3g in / $%.3g cached / $%.3g out per Mtok",
                     input_cost, cached_input_cost, output_cost);
        } else {
            snprintf(segment, sizeof(segment), "$%.3g in / $%.3g out per Mtok", input_cost,
                     output_cost);
        }
        append_segment(&description, segment);
    }
    /* Keep backend prose separate from structured metadata. */
    if (model->description && *model->description) {
        if (description.len)
            buf_append(&description, "\n", 1);
        buf_append_str(&description, model->description);
    }
    if (!description.len) {
        buf_free(&description);
        return NULL;
    }
    return buf_steal(&description);
}

/* Cancellation aborts a picker chain; PICK_NONE permits provider defaults; failure rolls back. */
enum pick_status {
    PICK_MADE,
    PICK_CANCELLED,
    PICK_NONE,
    PICK_FAILED,
};

struct value_pick_result {
    enum pick_status status;
    char *value; /* owned when non-NULL */
};

struct model_pick_result {
    enum pick_status status;
    char *model; /* owned when status is PICK_MADE */
    int explicit_choice;
};

static struct model_pick_result pick_model_from_list(struct provider *provider,
                                                     struct model_info *models, size_t model_count,
                                                     const char *current_model)
{
    if (config_bool_or("sort_models", provider->sort_models))
        qsort(models, model_count, sizeof(*models), compare_model_info);

    /* Batch catalog lookup avoids loading the snapshot once per model. */
    struct catalog_entry *catalog = NULL;
    if (provider->catalog_id && *provider->catalog_id) {
        const char **model_ids = xmalloc(model_count * sizeof(*model_ids));
        for (size_t i = 0; i < model_count; i++)
            model_ids[i] = models[i].id;
        catalog = xmalloc(model_count * sizeof(*catalog));
        catalog_lookup_many(provider->catalog_id, model_ids, model_count, catalog, NULL);
        free(model_ids);
    }

    struct picker_item *items = xcalloc(model_count, sizeof(*items));
    char **descriptions = xcalloc(model_count, sizeof(*descriptions));
    size_t initial = 0;
    for (size_t i = 0; i < model_count; i++) {
        descriptions[i] = model_desc_line(&models[i], catalog ? &catalog[i] : NULL);
        items[i].label = models[i].id;
        items[i].desc = descriptions[i];
        /* Known lack of tool support dims but does not hide the row; catalog capability is
         * advisory and unknown elsewhere. */
        items[i].dim = models[i].tools == PROVIDER_CAP_NO;
        items[i].detail = items[i].dim ? "no tool calling" : NULL;
        items[i].current = current_model && strcmp(models[i].id, current_model) == 0;
        if (items[i].current)
            initial = i;
    }
    struct picker_opts options = {
        .title = "select a model", .items = items, .n = model_count, .initial = initial};
    long selected_index = picker_run(&options);

    struct model_pick_result result = {.status = PICK_CANCELLED};
    if (selected_index >= 0) {
        result.status = PICK_MADE;
        result.model = xstrdup(models[selected_index].id);
        result.explicit_choice = 1;
        /* Publish metadata before freeing the list so the chained effort picker can use it. This
         * temporarily displaces live-model metadata and must be restored on cancellation. */
        model_meta_store(provider, &models[selected_index]);
    }
    for (size_t i = 0; i < model_count; i++)
        free(descriptions[i]);
    free(descriptions);
    free(items);
    free(catalog);
    return result;
}

/* Pick against `provider`, which may be prospective rather than live. */
static struct model_pick_result choose_model(struct agent_state *state, struct provider *provider,
                                             const char *current_model)
{
    struct model_pick_result result = {.status = PICK_NONE};
    const char *provider_name = provider->name ? provider->name : "?";
    struct model_info *models = NULL;
    size_t model_count = 0;

    /* Missing enumeration, fetch failure, and an empty catalog need different diagnostics. */
    if (!provider->list_models) {
        ui_note("%s can't list models — set one with HAX_MODEL or in config", provider_name);
        state->render->disp.trail = 1;
        return result;
    }
    /* Model enumeration can block, so expose progress and cancellation before the picker opens. */
    struct busy *busy = busy_begin("fetching models...");
    char *error = NULL;
    int list_result =
        provider->list_models(provider, &models, &model_count, &error, busy_tick, NULL);
    if (busy_end(busy)) {
        /* Discard a result that races cancellation; busy_end already rendered interruption. */
        model_info_free(models, model_count);
        free(error);
        state->render->disp.trail = 1;
        result.status = PICK_CANCELLED;
        return result;
    }
    if (list_result != 0) {
        /* Prefer the adapter's actionable diagnostic when available. */
        if (error)
            ui_error("%s", error);
        else
            ui_error("failed to list models for %s", provider_name);
        state->render->disp.trail = 1;
        result.status = PICK_FAILED;
        free(error);
        model_info_free(models, model_count);
        return result;
    }
    if (model_count == 0) {
        /* An empty catalog has no provider-independent remedy. */
        ui_note("%s has no models available", provider_name);
        state->render->disp.trail = 1;
        result.status = PICK_FAILED;
        model_info_free(models, model_count);
        return result;
    }
    if (model_count == 1) {
        /* One available model needs no picker and does not imply user intent. */
        result.status = PICK_MADE;
        result.model = xstrdup(models[0].id);
        model_meta_store(provider, &models[0]);
        model_info_free(models, model_count);
        return result;
    }

    result = pick_model_from_list(provider, models, model_count, current_model);
    model_info_free(models, model_count);
    return result;
}

/* Offer only effort levels accepted by `model`. A NULL value means provider default. */
static struct value_pick_result choose_effort(struct agent_state *state, struct provider *provider,
                                              const char *model, const char *current_effort,
                                              int announce_unavailable)
{
    struct value_pick_result result = {.status = PICK_NONE};
    struct effort_set levels;
    model_meta_efforts(provider, model, &levels);
    if (levels.count == 0) {
        if (announce_unavailable) {
            /* Distinguish a model-specific restriction from a provider without effort support. */
            const char *const *provider_efforts = NULL;
            if (provider->list_efforts && provider->list_efforts(provider, &provider_efforts) > 0)
                ui_note("%s doesn't take reasoning-effort levels", model ? model : "this model");
            else
                ui_note("the %s provider doesn't expose reasoning-effort levels",
                        provider->name ? provider->name : "?");
            state->render->disp.trail = 1;
        }
        return result;
    }

    struct picker_item *items = xcalloc(levels.count + 1, sizeof(*items));
    items[0].label = "default";
    items[0].desc = "Let the provider choose the reasoning effort";
    size_t initial = 0;
    for (size_t i = 0; i < levels.count; i++) {
        items[i + 1].label = levels.values[i];
        items[i + 1].current = current_effort && strcmp(levels.values[i], current_effort) == 0;
        if (items[i + 1].current)
            initial = i + 1;
    }
    struct picker_opts options = {
        .title = "select reasoning effort",
        .items = items,
        .n = levels.count + 1,
        .initial = initial,
    };
    long selected_index = picker_run(&options);
    free(items);

    if (selected_index < 0) {
        result.status = PICK_CANCELLED;
        return result;
    }
    result.status = PICK_MADE;
    /* Default omits effort; a literal ladder value such as "none" remains explicit. */
    if (selected_index > 0)
        result.value = xstrdup(levels.values[selected_index - 1]);
    return result;
}

/* NULL model or effort retains same-provider state; callers pass default sentinels when changing
 * provider. */
static void apply_selection_overrides(const char *provider_id, const char *model,
                                      const char *effort)
{
    config_preset_exit(CONFIG_TIER_RUN);
    config_set_override("provider", provider_id);
    if (model)
        config_set_override("model", model);
    if (effort)
        config_set_override("effort", effort);
}

static void persist_selection(struct agent_state *state, const char *provider_id, const char *model,
                              const char *effort, int model_discovered)
{
    /* Discovered models remain concrete this run but must be rediscovered next launch. */
    const char *stored_model = (model && model_discovered) ? CONFIG_VALUE_DEFAULT : model;
    if (config_persist_selection(provider_id, stored_model, effort) != 0) {
        /* The run override remains valid; warn once that persistence failed. */
        static int warned;
        if (!warned) {
            warned = 1;
            ui_note("couldn't save to state.json — this choice applies to this run only");
            state->render->disp.trail = 1;
        }
    }
}

/* Config writes invalidate the borrowed provider id, so copy it before committing. */
static char *current_provider_id(const struct provider *provider)
{
    const char *provider_id = agent_provider_id(provider);
    return xstrdup(provider_id ? provider_id : "");
}

/* Raw constructor diagnostics bypass disp; synchronize its trailing-row state afterward. */
static void sync_constructor_diagnostics(struct agent_state *state, unsigned long before)
{
    if (hax_diag_sequence() != before) {
        state->render->disp.held = 0;
        state->render->disp.trail = 1;
    }
}

/* ---------- public flows ---------- */

void select_effort(struct agent_state *state)
{
    struct provider *provider = state->provider;
    if (!provider) {
        ui_note("no provider selected — use /provider to choose one first");
        state->render->disp.trail = 1;
        return;
    }

    struct value_pick_result effort_pick =
        choose_effort(state, provider, state->session->model, state->session->effort, 1);
    if (effort_pick.status != PICK_MADE)
        return;

    /* Pin the provider with explicit effort so autoselection becomes stable. Default uses the
     * sentinel to shadow lower tiers. When exiting a preset, persist its effective model rather
     * than reviving the pre-preset stored model. */
    const char *preset = config_str("preset");
    char *model = (preset && *preset && state->session->model && *state->session->model)
                      ? xstrdup(state->session->model)
                      : NULL;
    const char *effort = effort_pick.value ? effort_pick.value : CONFIG_VALUE_DEFAULT;
    char *provider_id = current_provider_id(provider);
    struct config_snapshot *snapshot = config_snapshot_take();
    apply_selection_overrides(provider_id, model, effort);
    if (agent_apply_settings(state, provider, 1) != 0) {
        config_snapshot_restore(snapshot);
    } else {
        config_snapshot_free(snapshot);
        persist_selection(state, provider_id, model, effort, provider->model_discovered);
    }
    free(provider_id);
    free(model);
    free(effort_pick.value);
}

static void restore_model_metadata(struct provider *provider, struct model_info *saved_metadata,
                                   int had_saved_metadata, const char *model)
{
    if (had_saved_metadata)
        model_meta_store(provider, saved_metadata);
    else
        model_meta_refresh(provider, model);
    model_info_clear(saved_metadata);
}

void select_model(struct agent_state *state)
{
    struct provider *provider = state->provider;
    if (!provider) {
        ui_note("no provider selected — use /provider to choose one first");
        state->render->disp.trail = 1;
        return;
    }
    /* The candidate metadata needed by effort selection temporarily displaces the live model's;
     * snapshot it for cancellation. */
    struct model_info saved_metadata;
    int had_saved_metadata = model_meta_snapshot(provider, &saved_metadata);

    struct model_pick_result model_pick = choose_model(state, provider, state->session->model);
    if (!model_pick.model) {
        model_info_clear(&saved_metadata);
        return; /* cancelled or no menu — notes already printed */
    }

    /* Resolve effort before config writes invalidate borrowed values. */
    struct value_pick_result effort_pick =
        choose_effort(state, provider, model_pick.model, state->session->effort, 0);
    if (effort_pick.status == PICK_CANCELLED) {
        restore_model_metadata(provider, &saved_metadata, had_saved_metadata,
                               state->session->model);
        free(model_pick.model);
        return;
    }

    /* An explicit choice converts discovered server state into a persisted model preference. */
    int model_discovered = model_pick.explicit_choice ? 0 : provider->model_discovered;

    /* Pin the provider with an explicit model. Missing/default effort uses the sentinel so stale
     * lower-tier effort cannot leak into a provider that did not advertise it. */
    const char *effort = effort_pick.value ? effort_pick.value : CONFIG_VALUE_DEFAULT;
    char *provider_id = current_provider_id(provider);
    struct config_snapshot *snapshot = config_snapshot_take();
    apply_selection_overrides(provider_id, model_pick.model, effort);
    if (agent_apply_settings(state, provider, 1) != 0) {
        config_snapshot_restore(snapshot);
        restore_model_metadata(provider, &saved_metadata, had_saved_metadata,
                               state->session->model);
    } else {
        model_info_clear(&saved_metadata);
        provider->model_discovered = model_discovered;
        config_snapshot_free(snapshot);
        persist_selection(state, provider_id, model_pick.model, effort, model_discovered);
    }
    free(provider_id);
    free(model_pick.model);
    free(effort_pick.value);
}

static int compare_factory_names(const void *left, const void *right)
{
    const struct provider_factory *const *left_factory = left;
    const struct provider_factory *const *right_factory = right;
    return strcmp((*left_factory)->name, (*right_factory)->name);
}

struct provider_pick_result {
    const struct provider_factory *factory;
    int probe_available;
};

static struct provider_pick_result choose_provider_factory(const char *current_provider_id)
{
    size_t factory_count = 0;
    const struct provider_factory *const *registered_factories = provider_all(&factory_count);

    /* Picker order is alphabetical; registry order remains autoselect priority. */
    const struct provider_factory **factories = xmalloc(factory_count * sizeof(*factories));
    memcpy(factories, registered_factories, factory_count * sizeof(*factories));
    qsort(factories, factory_count, sizeof(*factories), compare_factory_names);

    /* Pre-picker work uses bounded timeouts; cancellation starts at the picker. */
    struct availability_result *availability = xcalloc(factory_count, sizeof(*availability));
    probe_availability(factories, factory_count, availability);

    /* Keep unavailable rows selectable because probe results are advisory and may become stale. */
    struct picker_item *items = xcalloc(factory_count, sizeof(*items));
    size_t initial = 0;
    for (size_t i = 0; i < factory_count; i++) {
        items[i].label = factories[i]->name;
        items[i].dim = !availability[i].available;
        items[i].detail = availability[i].reason;
        items[i].current =
            current_provider_id && strcmp(factories[i]->name, current_provider_id) == 0;
        if (items[i].current)
            initial = i;
    }
    struct picker_opts options = {
        .title = "select a provider",
        .items = items,
        .n = factory_count,
        .initial = initial,
    };
    long selected_index = picker_run(&options);

    struct provider_pick_result result = {0};
    if (selected_index >= 0) {
        result.factory = factories[selected_index];
        result.probe_available = availability[selected_index].available;
    }
    free(items);
    free(availability);
    free(factories);
    return result;
}

void select_provider(struct agent_state *state)
{
    char *current_id = state->provider ? current_provider_id(state->provider) : NULL;
    struct provider_pick_result provider_pick = choose_provider_factory(current_id);
    const struct provider_factory *factory = provider_pick.factory;
    if (!factory) {
        free(current_id);
        return; /* cancelled / non-tty — leave disp as the dispatcher's separator */
    }

    /* Recheck an unavailable row at commit because the advisory probe may be stale. */
    if (!provider_pick.probe_available && factory->prepare_availability) {
        const char *unavailable_reason = NULL;
        if (!factory_available(factory, &unavailable_reason)) {
            ui_note("%s is unavailable — %s", factory->name,
                    unavailable_reason ? unavailable_reason : "unavailable");
            state->render->disp.trail = 1;
            free(current_id);
            return;
        }
    }

    /* Re-picking the live provider avoids rebuilding it and continues to model selection. */
    if (current_id && strcmp(factory->name, current_id) == 0) {
        free(current_id);
        select_model(state);
        return;
    }

    /* Construct under a snapshotted prospective selection. Default sentinels prevent the old
     * backend's model and effort from influencing value-dependent constructors. */
    struct config_snapshot *snapshot = config_snapshot_take();
    config_set_override("provider", factory->name);
    config_set_override("model", CONFIG_VALUE_DEFAULT);
    config_set_override("effort", CONFIG_VALUE_DEFAULT);
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider *candidate = factory->new(factory->name);
    sync_constructor_diagnostics(state, diagnostics_before);
    if (!candidate) {
        config_snapshot_restore(snapshot);
        state->render->disp.trail = 1; /* the factory printed a raw error line */
        free(current_id);
        return;
    }

    /* Gather picks against the prospective provider before ownership transfer. PICK_NONE may use
     * its default model; cancellation and failure roll back silently or after their diagnostic. */
    struct model_pick_result model_pick = choose_model(state, candidate, NULL);
    int has_default_model = candidate->default_model && *candidate->default_model;
    if (!model_pick.model && (model_pick.status != PICK_NONE || !has_default_model)) {
        if (model_pick.status != PICK_CANCELLED) {
            ui_note("staying on %s — no model chosen for %s", current_id ? current_id : "?",
                    factory->name);
            state->render->disp.trail = 1;
        }
        candidate->destroy(candidate);
        config_snapshot_restore(snapshot);
        free(current_id);
        return;
    }

    /* Effort cancellation rolls back; default or no ladder commits the sentinel. */
    const char *model = model_pick.model ? model_pick.model : candidate->default_model;
    struct value_pick_result effort_pick = choose_effort(state, candidate, model, NULL, 0);
    if (effort_pick.status == PICK_CANCELLED) {
        candidate->destroy(candidate);
        config_snapshot_restore(snapshot);
        free(current_id);
        free(model_pick.model);
        return;
    }

    int model_discovered = model_pick.explicit_choice ? 0 : candidate->model_discovered;
    const char *model_override = model_pick.model ? model_pick.model : CONFIG_VALUE_DEFAULT;
    const char *effort_override = effort_pick.value ? effort_pick.value : CONFIG_VALUE_DEFAULT;
    apply_selection_overrides(factory->name, model_override, effort_override);

    if (agent_apply_settings(state, candidate, 1) != 0) {
        candidate->destroy(candidate); /* ownership transfers only on success */
        config_snapshot_restore(snapshot);
        free(current_id);
        free(model_pick.model);
        free(effort_pick.value);
        return;
    }
    candidate->model_discovered = model_discovered;
    config_snapshot_free(snapshot);
    persist_selection(state, factory->name, model_override, effort_override, model_discovered);

    free(current_id);
    free(model_pick.model);
    free(effort_pick.value);
}

static char *choose_preset_name(char **names, size_t count)
{
    qsort(names, count, sizeof(*names), compare_string_pointers);
    struct picker_item *items = xcalloc(count, sizeof(*items));
    char **details = xcalloc(count, sizeof(*details));
    const char *active_preset = config_str("preset");
    size_t initial = 0;
    for (size_t i = 0; i < count; i++) {
        items[i].label = names[i];
        items[i].desc = config_preset_description(names[i]);
        /* Preview the preset's tint, falling back below run overrides because applying the
         * preset clears the current stance. */
        const char *tint = config_preset_tint(names[i]);
        items[i].label_color = theme_tint_open(tint ? tint : config_str_below_run("tint"));
        items[i].current = active_preset && *active_preset && strcmp(active_preset, names[i]) == 0;
        if (items[i].current)
            initial = i;

        /* Show only explicit preset fields; resolving defaults would require construction. */
        const char *provider_id = config_preset_provider(names[i]);
        if (provider_id && provider_find(provider_id)) {
            struct buf detail;
            buf_init(&detail);
            append_segment(&detail, provider_id);
            const char *model = config_preset_model(names[i]);
            const char *effort = config_preset_effort(names[i]);
            if (model && *model)
                append_segment(&detail, model);
            if (effort && *effort)
                append_segment(&detail, effort);
            details[i] = buf_steal(&detail);
        } else {
            /* Unknown provider ids are configuration defects, not availability failures. Keep
             * them selectable so commit can report the same error. */
            details[i] = xasprintf("unknown provider '%s'", provider_id ? provider_id : "?");
            items[i].dim = 1;
        }
        items[i].detail = details[i];
    }
    struct picker_opts options = {
        .title = "select a preset", .items = items, .n = count, .initial = initial};
    long selected_index = picker_run(&options);
    char *selected_name = selected_index >= 0 ? xstrdup(names[selected_index]) : NULL;
    free(items);
    for (size_t i = 0; i < count; i++)
        free(details[i]);
    free(details);
    return selected_name;
}

int select_preset(struct agent_state *state, const char *name, int announce)
{
    char **names = NULL;
    size_t preset_count = config_preset_names(&names);
    char *selected_name = NULL;
    int result = -1;

    if (!name) {
        if (preset_count == 0) {
            ui_note("no presets defined in config.json — use /preset-save to save the current "
                    "selection");
            state->render->disp.trail = 1;
            goto out;
        }
        selected_name = choose_preset_name(names, preset_count);
        if (!selected_name)
            goto out; /* cancelled / non-tty */
        name = selected_name;
    }

    /* Preset application is transactional across overrides and provider construction. */
    struct config_snapshot *snapshot = config_snapshot_take();
    char *error = NULL;
    if (config_preset_apply(name, CONFIG_TIER_RUN, &error) != 0) {
        ui_error("%s", error ? error : "preset failed to apply");
        free(error);
        config_snapshot_restore(snapshot);
        state->render->disp.trail = 1;
        goto out;
    }

    /* Always construct under preset overrides, even for the live provider id; value-dependent
     * reconciliation occurs during construction. */
    const char *provider_id = config_str("provider");
    const struct provider_factory *factory = provider_find(provider_id);
    if (!factory) {
        ui_error("preset '%s': unknown provider '%s'", name, provider_id);
        config_snapshot_restore(snapshot);
        state->render->disp.trail = 1;
        goto out;
    }
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider *candidate = factory->new(factory->name);
    sync_constructor_diagnostics(state, diagnostics_before);
    if (!candidate) {
        /* The constructor already diagnosed the failure. */
        config_snapshot_restore(snapshot);
        state->render->disp.trail = 1;
        goto out;
    }

    /* Validate the post-construction model before ownership transfer; construction may have
     * reconciled a discovered model into the override tier. */
    const char *model = config_str("model");
    if ((!model || !*model) && !(candidate->default_model && *candidate->default_model)) {
        ui_error("preset '%s': no model resolves for provider '%s' — name one in the preset", name,
                 factory->name);
        candidate->destroy(candidate);
        config_snapshot_restore(snapshot);
        state->render->disp.trail = 1;
        goto out;
    }

    /* Ownership transfers only after validation; unexpected apply failure still rolls back. */
    if (agent_apply_settings(state, candidate, announce) != 0) {
        candidate->destroy(candidate);
        config_snapshot_restore(snapshot);
        state->render->disp.trail = 1;
        goto out;
    }
    config_snapshot_free(snapshot);

    /* Persist the name only after application succeeds, keeping its definition authoritative. */
    if (config_persist_state("preset", name) != 0) {
        static int warned;
        if (!warned) {
            warned = 1;
            ui_note("couldn't save to state.json — this preset applies to this run only");
            state->render->disp.trail = 1;
        }
    }
    result = 0;

out:
    free(selected_name);
    for (size_t i = 0; i < preset_count; i++)
        free(names[i]);
    free(names);
    return result;
}

/* ---------- saving the live selection as a preset ---------- */

/* A NULL value means the preset carries no tint. */
static struct value_pick_result choose_tint(const char *current_tint)
{
    struct value_pick_result result = {.status = PICK_NONE};
    const struct config_setting *setting = config_setting_find("tint");
    if (!setting || !setting->choices)
        return result;

    char **values = NULL;
    size_t value_count = split_choices(setting->choices, &values);
    struct picker_item *items = xcalloc(value_count + 1, sizeof(*items));
    items[0].label = "none";
    items[0].desc = "Carry no tint of its own: your tint setting applies";
    items[0].current = !current_tint;
    size_t initial = 0;
    for (size_t i = 0; i < value_count; i++) {
        items[i + 1].label = values[i];
        items[i + 1].label_color = theme_tint_open(values[i]);
        /* Keep hand-written case variants selected. */
        items[i + 1].current = current_tint && strcasecmp(values[i], current_tint) == 0;
        if (items[i + 1].current)
            initial = i + 1;
    }
    struct picker_opts options = {
        .title = "tint for this preset",
        .items = items,
        .n = value_count + 1,
        .initial = initial,
    };
    long selected_index = picker_run(&options);
    free(items);
    if (selected_index >= 0) {
        result.status = PICK_MADE;
        if (selected_index > 0)
            result.value = xstrdup(values[selected_index - 1]);
    } else {
        result.status = PICK_CANCELLED;
    }
    for (size_t i = 0; i < value_count; i++)
        free(values[i]);
    free(values);
    return result;
}

/* Require explicit confirmation before replacing an existing preset; cancellation declines. */
static int confirm_overwrite(const char *name)
{
    struct buf current_selection;
    buf_init(&current_selection);
    const char *provider_id = config_preset_provider(name);
    append_segment(&current_selection, provider_id && *provider_id ? provider_id : "no provider");
    const char *model = config_preset_model(name);
    const char *effort = config_preset_effort(name);
    if (model && *model)
        append_segment(&current_selection, model);
    if (effort && *effort)
        append_segment(&current_selection, effort);
    char *selection_detail = buf_steal(&current_selection);

    struct picker_item items[] = {
        {.label = "keep it", .desc = "Leave the existing definition alone", .current = 1},
        {.label = "overwrite",
         .detail = selection_detail,
         .desc = "Replace it with the current selection"},
    };
    char *title = xasprintf("preset '%s' already exists", name);
    struct picker_opts options = {
        .title = title, .items = items, .n = sizeof(items) / sizeof(*items), .initial = 0};
    long selected_index = picker_run(&options);
    free(title);
    free(selection_detail);
    return selected_index == 1;
}

/* Save only a system prompt that normal resolution would not reproduce; copying config/default
 * values would become stale. */
static const char *capture_system_prompt(void)
{
    const char *system_prompt = config_str("system_prompt");
    if (!system_prompt)
        return NULL;
    const char *source = config_source("system_prompt");
    if (strcmp(source, "config") == 0 || strcmp(source, "default") == 0)
        return NULL;
    return system_prompt;
}

static const char *preset_save_initial_tint(const char *name, int preset_exists)
{
    if (strcmp(config_source("tint"), "run") == 0)
        return config_str("tint");
    const char *active_preset = config_str("preset");
    if (active_preset && *active_preset) {
        const char *tint = config_preset_tint(active_preset);
        if (tint)
            return tint;
    }
    return preset_exists ? config_preset_tint(name) : NULL;
}

void select_preset_save(struct agent_state *state, const char *argument)
{
    struct provider *provider = state->provider;
    if (!provider) {
        ui_note("no provider selected — use /provider to choose one first");
        state->render->disp.trail = 1;
        return;
    }
    /* A model-less preset could resolve differently from the live session. */
    if (!state->session->model || !*state->session->model) {
        ui_note("no model resolved yet — use /model to pick one first");
        state->render->disp.trail = 1;
        return;
    }
    /* Missing names return to editable input rather than failing. */
    if (!argument || !*argument) {
        ui_note("name it: /preset-save <name> [tint]");
        free(state->pending_preseed);
        state->pending_preseed = xstrdup("/preset-save ");
        state->render->disp.trail = 1;
        return;
    }

    const char *separator = argument;
    while (*separator && !isspace((unsigned char)*separator))
        separator++;
    size_t name_length = (size_t)(separator - argument);
    char *name = xmalloc(name_length + 1);
    memcpy(name, argument, name_length);
    name[name_length] = '\0';
    while (*separator && isspace((unsigned char)*separator))
        separator++;
    const char *tint_argument = *separator ? separator : NULL;
    char *tint = NULL;

    if (!config_preset_name_valid(name)) {
        ui_error("'%s' can't be a preset name — use letters, digits, '.', '-' or '_', starting "
                 "with a letter or digit",
                 name);
        state->render->disp.trail = 1;
        goto out;
    }
    const struct config_setting *tint_setting = config_setting_find("tint");
    if (tint_argument && !config_value_valid(tint_setting, tint_argument)) {
        ui_error("unknown tint '%s' (expected %s)", tint_argument,
                 tint_setting ? tint_setting->choices : "");
        state->render->disp.trail = 1;
        goto out;
    }

    int preset_exists = config_preset_exists(name);
    if (preset_exists && !confirm_overwrite(name)) {
        ui_note("left preset '%s' unchanged", name);
        state->render->disp.trail = 1;
        goto out;
    }

    if (tint_argument) {
        /* Persist canonical tint spelling after case-insensitive validation. */
        char *canonical_tint = config_value_canonical(tint_setting, tint_argument);
        tint = canonical_tint ? canonical_tint : xstrdup(tint_argument);
    } else {
        const char *initial_tint = preset_save_initial_tint(name, preset_exists);
        struct value_pick_result tint_pick = choose_tint(initial_tint);
        if (tint_pick.status == PICK_CANCELLED)
            goto out;
        tint = tint_pick.value;
    }

    struct config_preset definition = {
        .provider = agent_provider_id(provider),
        /* Omit discovered server state so applying the preset re-discovers the model. */
        .model = provider->model_discovered ? NULL : state->session->model,
        .effort = state->session->effort,
        .system_prompt = capture_system_prompt(),
        .tint = tint,
        /* Preserve the existing description on re-save. */
        .description = preset_exists ? config_preset_description(name) : NULL,
    };
    char *error = NULL;
    if (config_preset_save(name, &definition, &error) != 0) {
        ui_error("%s", error ? error : "couldn't save the preset");
        free(error);
        state->render->disp.trail = 1;
        goto out;
    }
    ui_note("%s preset '%s' in config.json", preset_exists ? "updated" : "saved", name);
    state->render->disp.trail = 1;

    /* Enter the saved stance so its name and tint become active and persistent. */
    select_preset(state, name, 1);

out:
    free(tint);
    free(name);
}

/* ---------- restoring a resumed conversation's selection ---------- */

/* Equal, treating NULL and "" as the same absence. */
static int selection_value_equal(const char *left, const char *right)
{
    if (!left || !*left)
        return !right || !*right;
    return right && strcmp(left, right) == 0;
}

void select_restore_session(struct agent_state *state, const char *provider_id, const char *model,
                            const char *effort, const char *preset)
{
    /* A provider-less recording leaves the live provider unchanged. */
    if (!provider_id || !*provider_id || strcmp(provider_id, "none") == 0)
        return;

    struct provider *live_provider = state->provider;
    char *current_id = current_provider_id(live_provider);
    if (live_provider && selection_value_equal(current_id, provider_id) &&
        selection_value_equal(state->session->model, model) &&
        selection_value_equal(state->session->effort, effort) &&
        selection_value_equal(config_str("preset"), preset)) {
        free(current_id);
        return;
    }

    /* Restore into run overrides so it outranks earlier selectors without becoming a persisted
     * default. Roll back the tier if provider restoration fails. */
    struct config_snapshot *snapshot = config_snapshot_take();
    char *error = NULL;
    if (config_restore_selection(CONFIG_TIER_RUN, provider_id, model, effort, preset, &error) !=
        0) {
        /* A missing preset does not prevent restoring its recorded provider and model. */
        ui_note("%s — resuming without it", error ? error : "preset failed to apply");
        state->render->disp.trail = 1;
        free(error);
    }

    /* Reconstruct even the same provider id so value-dependent setup runs under restored values. */
    const char *restored_provider_id = config_str("provider");
    const struct provider_factory *factory = provider_find(restored_provider_id);
    const char *display_provider_id = factory ? factory->name : restored_provider_id;
    unsigned long diagnostics_before = hax_diag_sequence();
    struct provider *candidate = factory ? factory->new(factory->name) : NULL;
    sync_constructor_diagnostics(state, diagnostics_before);
    if (!candidate) {
        if (!factory)
            ui_error("session used unknown provider '%s'", display_provider_id);
        /* Do not silently move restored history to another backend. */
        ui_note("couldn't restore %s — staying on %s (use /provider to switch)",
                display_provider_id, current_id ? current_id : "no provider");
        state->render->disp.trail = 1;
        config_snapshot_restore(snapshot);
        free(current_id);
        return;
    }
    /* Validate the model before transferring provider ownership. */
    const char *restored_model = config_str("model");
    if ((!restored_model || !*restored_model) &&
        !(candidate->default_model && *candidate->default_model)) {
        ui_note("couldn't restore %s — no model resolves for it; staying on %s",
                display_provider_id, current_id ? current_id : "no provider");
        state->render->disp.trail = 1;
        candidate->destroy(candidate);
        config_snapshot_restore(snapshot);
        free(current_id);
        return;
    }
    if (agent_apply_settings(state, candidate, 1) != 0) {
        candidate->destroy(candidate);
        config_snapshot_restore(snapshot);
        state->render->disp.trail = 1;
        free(current_id);
        return;
    }
    config_snapshot_free(snapshot);
    free(current_id);
}

/* ---------- /config ---------- */

static int setting_is_bool(const struct config_setting *setting)
{
    return setting->kind == CONFIG_KIND_STRING && setting->choices &&
           strcmp(setting->choices, CONFIG_CHOICES_BOOL) == 0;
}

/* "auto" defers to a consumer-specific default that /config cannot resolve to on or off. */
static int setting_is_tristate(const struct config_setting *setting)
{
    return setting->kind == CONFIG_KIND_STRING && setting->choices &&
           strcmp(setting->choices, CONFIG_CHOICES_TRISTATE) == 0;
}

/* Return a borrowed display-safe value: redact secrets, normalize booleans, and honor registry
 * empty-value policy. Invalidated by the next override write. */
static const char *setting_display_value(const struct config_setting *setting)
{
    const char *value = config_str(setting->key);
    if (setting->secret)
        return (value && *value) ? "set" : "unset";
    if (setting_is_bool(setting))
        return config_bool(setting->key) ? "on" : "off";
    if (setting_is_tristate(setting)) {
        /* Preserve invalid raw values for the diagnostic marker; normalize valid aliases. */
        if (!value || strcasecmp(value, "auto") == 0)
            return "auto";
        if (!config_value_valid(setting, value))
            return value;
        return config_bool_or(setting->key, 0) ? "on" : "off";
    }
    if (!value)
        return "unset";
    /* A resolved empty value is meaningful only for keep_empty settings; distinguish it from
     * unset. */
    if (!*value)
        return "(empty)";
    return value;
}

/* Detect configured values rejected by typed consumers, excluding secrets. */
static int setting_value_invalid(const struct config_setting *setting)
{
    const char *value = config_str(setting->key);
    return value && *value && !setting->secret && !config_value_valid(setting, value);
}

/* Mark rejected configured values so they do not appear effective. */
static void note_current_setting(struct agent_state *state, const struct config_setting *setting)
{
    ui_note("%s = %s (%s%s)", setting->key, setting_display_value(setting),
            config_source(setting->key), setting_value_invalid(setting) ? ", invalid" : "");
    state->render->disp.trail = 1;
}

/* Keep slash-command knowledge out of the config layer. */
static const char *setting_runtime_command(const char *key)
{
    static const struct {
        const char *key;
        const char *command;
    } commands[] = {
        {"provider", "/provider"},
        {"model", "/model"},
        {"effort", "/effort"},
        {"preset", "/preset"},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strcmp(commands[i].key, key) == 0)
            return commands[i].command;
    return NULL;
}

static void note_readonly_setting(struct agent_state *state, const struct config_setting *setting)
{
    note_current_setting(state, setting);
    const char *command = setting_runtime_command(setting->key);
    if (command)
        ui_note("  change it with %s", command);
    else
        ui_note("  read-only at runtime — set %s or config.json and restart to change",
                setting->env_var);
}

static void commit_setting(struct agent_state *state, const struct config_setting *setting,
                           const char *value)
{
    config_set_override(setting->key, value);
    agent_display_refresh(state);
    note_current_setting(state, setting);
}

static void preseed_setting(struct agent_state *state, const struct config_setting *setting,
                            const char *value)
{
    free(state->pending_preseed);
    state->pending_preseed = (value && *value) ? xasprintf("/config %s %s", setting->key, value)
                                               : xasprintf("/config %s ", setting->key);
}

/* "default" clears the override. Numeric choice lists also hand off to editable exact input. */
static void pick_setting_choice(struct agent_state *state, const struct config_setting *setting)
{
    char **values = NULL;
    size_t value_count = split_choices(setting->choices, &values);
    int accepts_exact_value = setting->kind != CONFIG_KIND_STRING;
    size_t item_count = value_count + 1 + (accepts_exact_value ? 1 : 0);

    struct picker_item *items = xcalloc(item_count, sizeof(*items));
    items[0].label = "default";
    items[0].desc = "Clear the runtime override and use the environment, saved configuration, or "
                    "built-in default";
    const char *current_value = setting_display_value(setting);
    size_t initial = 0;
    int choice_is_current = 0;
    for (size_t i = 0; i < value_count; i++) {
        items[i + 1].label = values[i];
        /* Preview tint choices in their colors. */
        if (strcmp(setting->key, "tint") == 0)
            items[i + 1].label_color = theme_tint_open(values[i]);
        items[i + 1].current = strcasecmp(values[i], current_value) == 0;
        if (items[i + 1].current) {
            initial = i + 1;
            choice_is_current = 1;
        }
    }

    int exact_value_is_current =
        accepts_exact_value && !choice_is_current && config_value_valid(setting, current_value);
    char *exact_value_description = NULL;
    if (accepts_exact_value) {
        items[value_count + 1].label = "exact value...";
        if (setting->example) {
            exact_value_description =
                xasprintf("Enter an exact value such as %s", setting->example);
            items[value_count + 1].desc = exact_value_description;
        }
        items[value_count + 1].current = exact_value_is_current;
        if (exact_value_is_current)
            initial = value_count + 1;
    }

    char *title = xasprintf("%s — %s", setting->key, setting->description);
    struct picker_opts options = {
        .title = title, .items = items, .n = item_count, .initial = initial};
    long selected_index = picker_run(&options);
    free(title);
    free(exact_value_description);
    free(items);

    if (selected_index == 0)
        commit_setting(state, setting, NULL);
    else if (selected_index > 0 && (size_t)selected_index <= value_count)
        commit_setting(state, setting, values[selected_index - 1]);
    else if (accepts_exact_value && (size_t)selected_index == value_count + 1)
        preseed_setting(state, setting, exact_value_is_current ? current_value : setting->example);
    for (size_t i = 0; i < value_count; i++)
        free(values[i]);
    free(values);
}

/* Seed invalid current values from the registry default rather than resubmitting them. */
static void seed_setting_prompt(struct agent_state *state, const struct config_setting *setting)
{
    const char *value = config_str(setting->key);
    if (value && *value && !config_value_valid(setting, value))
        value = config_default(setting->key);
    preseed_setting(state, setting, value);
}

static void select_config_argument(struct agent_state *state, const char *argument)
{
    const char *separator = argument;
    while (*separator && !isspace((unsigned char)*separator))
        separator++;
    char key[64];
    size_t key_length = (size_t)(separator - argument);
    if (key_length >= sizeof(key)) {
        ui_error("unknown setting '%.*s'", (int)key_length, argument);
        state->render->disp.trail = 1;
        return;
    }
    memcpy(key, argument, key_length);
    key[key_length] = '\0';
    while (*separator && isspace((unsigned char)*separator))
        separator++;
    const char *value = *separator ? separator : NULL;

    const struct config_setting *setting = config_setting_find(key);
    if (!setting) {
        ui_error("unknown setting '%s' — /config lists them", key);
        state->render->disp.trail = 1;
        return;
    }
    if (!value) {
        if (setting->editable)
            note_current_setting(state, setting);
        else
            note_readonly_setting(state, setting);
        return;
    }
    if (!setting->editable) {
        const char *command = setting_runtime_command(key);
        if (command)
            ui_error("'%s' can't be changed from /config — use %s", key, command);
        else
            ui_error("'%s' can't be changed at runtime — set %s or config.json and restart", key,
                     setting->env_var);
        state->render->disp.trail = 1;
        return;
    }
    if (strcmp(value, "default") == 0) {
        commit_setting(state, setting, NULL);
        return;
    }
    if (!config_value_valid(setting, value)) {
        char hint[64];
        config_value_hint(setting, hint, sizeof(hint));
        ui_error("invalid value '%s' for %s (expected: %s, or default)", value, key, hint);
        state->render->disp.trail = 1;
        return;
    }
    /* Store the canonical spelling so a case-sensitive consumer matches. */
    char *canonical_value = config_value_canonical(setting, value);
    commit_setting(state, setting, canonical_value ? canonical_value : value);
    free(canonical_value);
}

static const struct config_setting *choose_config_setting(void)
{
    /* Preserve registry grouping; dim rows are inspectable but read-only. */
    size_t setting_count = 0;
    const struct config_setting *settings = config_settings(&setting_count);

    struct picker_item *items = xcalloc(setting_count, sizeof(*items));
    char **details = xmalloc(setting_count * sizeof(*details));
    char **descriptions = xcalloc(setting_count, sizeof(*descriptions));
    for (size_t i = 0; i < setting_count; i++) {
        const struct config_setting *setting = &settings[i];
        details[i] =
            xasprintf("%s (%s%s)", setting_display_value(setting), config_source(setting->key),
                      setting_value_invalid(setting) ? ", invalid" : "");
        items[i].label = setting->key;
        items[i].detail = details[i];
        /* Show units or bounds only when the value grammar adds useful information. */
        int show_hint = setting->kind == CONFIG_KIND_SIZE ||
                        setting->kind == CONFIG_KIND_DURATION ||
                        (setting->kind == CONFIG_KIND_INT && (setting->min || setting->max));
        if (show_hint) {
            char hint[64];
            config_value_hint(setting, hint, sizeof(hint));
            descriptions[i] = xasprintf("%s (%s)", setting->description, hint);
            items[i].desc = descriptions[i];
        } else {
            items[i].desc = setting->description;
        }
        items[i].dim = !setting->editable;
    }
    struct picker_opts options = {
        .title = "configuration", .items = items, .n = setting_count, .initial = 0};
    long selected_index = picker_run(&options);
    free(items);
    for (size_t i = 0; i < setting_count; i++) {
        free(details[i]);
        free(descriptions[i]);
    }
    free(details);
    free(descriptions);

    return selected_index >= 0 ? &settings[selected_index] : NULL;
}

void select_config(struct agent_state *state, const char *argument)
{
    if (argument && *argument) {
        select_config_argument(state, argument);
        return;
    }

    const struct config_setting *setting = choose_config_setting();
    if (!setting)
        return;
    if (!setting->editable)
        note_readonly_setting(state, setting);
    else if (setting->choices)
        pick_setting_choice(state, setting);
    else
        seed_setting_prompt(state, setting);
}
