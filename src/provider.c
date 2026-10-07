/* SPDX-License-Identifier: MIT */
#include "provider.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"
#include "text/json_scan.h"

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

int item_is_typed_prompt(const struct item *item)
{
    return item->kind == ITEM_USER_MESSAGE && item->origin == ITEM_ORIGIN_NONE;
}

size_t items_user_turn_cut(const struct item *items, size_t n_items, size_t keep_user_turns)
{
    size_t prompts_seen = 0;
    for (size_t i = 0; i < n_items; i++) {
        if (!item_is_typed_prompt(&items[i]))
            continue;
        if (prompts_seen == keep_user_turns)
            return i > 0 && items[i - 1].kind == ITEM_TURN_BOUNDARY ? i - 1 : i;
        prompts_seen++;
    }
    return n_items;
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
    free(item->reasoning_field);
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

enum provider_cap provider_cap_listed(const json_t *list, const char *value)
{
    if (!json_is_array(list))
        return PROVIDER_CAP_UNKNOWN;
    size_t index;
    json_t *member;
    json_array_foreach(list, index, member)
    {
        const char *text = json_string_value(member);
        if (text && strcmp(text, value) == 0)
            return PROVIDER_CAP_YES;
    }
    return PROVIDER_CAP_NO;
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

void model_probe_parse(const struct model_probe *probe, const char *body, const char *model,
                       struct model_info *out, char ***ids)
{
    if (ids)
        *ids = NULL;
    if (probe->parse) {
        probe->parse(body, model, out);
        return;
    }

    /* Listings can run to megabytes once tree-parsed, so parse one entry at a time. */
    struct json_scan_entry list;
    struct json_scan entries;
    if (json_scan_find(body, probe->list_member ? probe->list_member : "data", &list) != 1 ||
        json_scan_array(&entries, list.value) != 0)
        return;
    const char *id_member = probe->id_member ? probe->id_member : "id";
    char **listed = NULL;
    size_t listed_count = 0;
    size_t listed_capacity = 0;
    int found = 0;
    struct json_scan_entry element;
    int scanned;
    while ((scanned = json_scan_next(&entries, &element)) == 1) {
        json_t *entry = json_scan_load(&element);
        if (!entry) {
            scanned = -1;
            break;
        }
        const char *id = json_string_value(json_object_get(entry, id_member));
        if (id && model && probe->parse_entry && !found && strcmp(id, model) == 0) {
            probe->parse_entry(entry, out);
            found = 1;
        }
        if (ids && id && *id && !(probe->entry_hidden && probe->entry_hidden(entry))) {
            /* Room for the terminator. */
            if (listed_count + 1 >= listed_capacity) {
                listed_capacity = listed_capacity ? listed_capacity * 2 : 64;
                listed = xrealloc(listed, listed_capacity * sizeof(*listed));
            }
            listed[listed_count++] = xstrdup(id);
        }
        json_decref(entry);
        if (found && !ids)
            break;
    }

    if (!ids)
        return;
    if (listed)
        listed[listed_count] = NULL;
    /* A partial listing would pass for the whole one. */
    if (scanned < 0)
        string_array_free(listed);
    else
        *ids = listed;
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
