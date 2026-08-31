/* SPDX-License-Identifier: MIT */
#include "provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

char *item_image_placeholder(const struct item_image *image)
{
    /* Padding can overstate the decoded size by at most two bytes, which is
     * immaterial at the displayed precision. */
    size_t bytes = image->data_b64 ? strlen(image->data_b64) / 4 * 3 : 0;
    char size[32];

    if (bytes >= 1024 * 1024)
        snprintf(size, sizeof(size), "%.1f MiB", (double)bytes / (1024 * 1024));
    else if (bytes >= 1024)
        snprintf(size, sizeof(size), "%.1f KiB", (double)bytes / 1024);
    else
        snprintf(size, sizeof(size), "%zu bytes", bytes);

    const char *mime = image->mime ? image->mime : "image";
    if (image->width > 0 && image->height > 0)
        return xasprintf("[image: %s, %ldx%ld, %s]", mime, image->width, image->height, size);
    return xasprintf("[image: %s, %s]", mime, size);
}

size_t items_image_base64_bytes(const struct item *items, size_t n_items)
{
    size_t total = 0;

    for (size_t i = 0; i < n_items; i++)
        for (size_t j = 0; j < items[i].n_images; j++)
            total += items[i].images[j].data_b64 ? strlen(items[i].images[j].data_b64) : 0;
    return total;
}

size_t items_image_count(const struct item *items, size_t n_items)
{
    size_t total = 0;

    for (size_t i = 0; i < n_items; i++)
        total += items[i].n_images;
    return total;
}

size_t items_context_floor(const struct item *items, size_t n_items)
{
    for (size_t i = n_items; i-- > 0;)
        if (items[i].kind == ITEM_USER_MESSAGE && items[i].origin == ITEM_ORIGIN_COMPACT_SEED)
            return i;
    return 0;
}

void item_free(struct item *item)
{
    if (!item)
        return;

    free(item->text);
    free(item->call_id);
    free(item->tool_name);
    free(item->tool_arguments_json);
    free(item->output);
    for (size_t i = 0; i < item->n_images; i++) {
        free(item->images[i].mime);
        free(item->images[i].data_b64);
    }
    free(item->images);
    free(item->reasoning_json);
    free(item->reasoning_text);
    free(item->provider);
    free(item->model);
    turn_usage_free(item->usage);
    memset(item, 0, sizeof(*item));
}

void turn_usage_free(struct turn_usage *usage)
{
    if (!usage)
        return;
    free(usage->provenance.provider_label);
    free(usage->provenance.model_label);
    free(usage->provenance.effort);
    free(usage->provenance.served_model);
    free(usage->provenance.route);
    free(usage->provenance.response_id);
    free(usage);
}

void model_info_init(struct model_info *info)
{
    memset(info, 0, sizeof(*info));
    info->cost_input = -1;
    info->cost_cache_read = -1;
    info->cost_output = -1;
    info->cost_cache_write = -1;
    info->cost_cache_write_1h = -1;
}

void model_info_copy(struct model_info *dst, const struct model_info *src)
{
    *dst = *src;
    dst->id = src->id ? xstrdup(src->id) : NULL;
    dst->description = src->description ? xstrdup(src->description) : NULL;
}

void model_info_clear(struct model_info *info)
{
    free(info->id);
    free(info->description);
    memset(info, 0, sizeof(*info));
}

void model_info_free(struct model_info *models, size_t n_models)
{
    if (!models)
        return;
    for (size_t i = 0; i < n_models; i++)
        model_info_clear(&models[i]);
    free(models);
}

void model_probe_clear(struct model_probe *probe)
{
    if (!probe)
        return;
    free(probe->url);
    string_array_free(probe->headers);
    memset(probe, 0, sizeof(*probe));
}

const char *provider_canonical_id(const char *name)
{
    /* Former id of the llamacpp provider ('.' clashes with the config key path separator). */
    if (name && strcmp(name, "llama.cpp") == 0)
        return "llamacpp";
    return name;
}

const char *provider_stable_id(const struct provider *provider)
{
    return provider->id && *provider->id ? provider->id : provider->name;
}

int provider_provenance_matches(const struct item *item, const char *provider, const char *model)
{
    if (!item->provider || !item->model || !provider || !model)
        return 0;
    return strcmp(provider_canonical_id(item->provider), provider_canonical_id(provider)) == 0 &&
           strcmp(item->model, model) == 0;
}

void provider_availability_clear(struct provider_availability *availability)
{
    if (!availability)
        return;
    free(availability->reason);
    free(availability->url);
    string_array_free(availability->headers);
    memset(availability, 0, sizeof(*availability));
}
