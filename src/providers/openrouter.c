/* SPDX-License-Identifier: MIT */
#include "providers/openrouter.h"

#include <errno.h>
#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "busy.h"
#include "catalog.h"
#include "effort.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/provider_config.h"
#include "render/ctrl_strip.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "text/fmt.h"
#include "transport/http.h"

#define OPENROUTER_BASE_URL         "https://openrouter.ai/api/v1"
#define OPENROUTER_MODELS_ENDPOINT  OPENROUTER_BASE_URL "/models"
#define OPENROUTER_KEY_ENDPOINT     OPENROUTER_BASE_URL "/key"
#define OPENROUTER_CREDITS_ENDPOINT OPENROUTER_BASE_URL "/credits"

#define MODEL_PROBE_TIMEOUT_S 5
#define USAGE_TIMEOUT_S       30

static const char *openrouter_api_key(void)
{
    return provider_api_key("providers.openrouter", "OPENROUTER_API_KEY");
}

/* A missing or malformed capability list is unknown, not unsupported. */
static enum provider_cap capability_from_array(const json_t *array, const char *value)
{
    if (!json_is_array(array))
        return PROVIDER_CAP_UNKNOWN;
    for (size_t i = 0; i < json_array_size(array); i++) {
        const char *item = json_string_value(json_array_get(array, i));
        if (item && strcmp(item, value) == 0)
            return PROVIDER_CAP_YES;
    }
    return PROVIDER_CAP_NO;
}

/* OpenRouter reports USD per token; model_info stores USD per million tokens. */
static double parse_token_rate(const json_t *pricing, const char *key)
{
    const char *text = json_string_value(json_object_get(pricing, key));
    if (!text || !*text)
        return -1;

    errno = 0;
    char *end = NULL;
    double rate = strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE || !isfinite(rate) || rate < 0)
        return -1;

    rate *= 1e6;
    return isfinite(rate) ? rate : -1;
}

static char *parse_description_lead(const json_t *entry)
{
    const char *description = json_string_value(json_object_get(entry, "description"));
    if (!description)
        return NULL;

    size_t length = strcspn(description, "\r\n");
    while (length > 0 && (description[length - 1] == ' ' || description[length - 1] == '\t'))
        length--;
    if (length == 0)
        return NULL;

    char *lead = xmalloc(length + 1);
    memcpy(lead, description, length);
    lead[length] = '\0';
    return lead;
}

/* OpenRouter and catalog_tier both define the threshold as exclusive. */
static void parse_pricing_tiers(const json_t *pricing, struct model_info *info)
{
    json_t *overrides = json_object_get(pricing, "overrides");
    if (!json_is_array(overrides))
        return;

    size_t index;
    json_t *override;
    json_array_foreach(overrides, index, override)
    {
        if (info->n_tiers >= CATALOG_TIERS_MAX)
            break;
        if (!json_is_object(override))
            continue;

        json_t *threshold = json_object_get(override, "min_prompt_tokens");
        if (!json_is_integer(threshold) || json_integer_value(threshold) <= 0)
            continue;

        struct catalog_tier *tier = &info->tiers[info->n_tiers++];
        tier->context_threshold = (long)json_integer_value(threshold);
        tier->cost_input = parse_token_rate(override, "prompt");
        tier->cost_output = parse_token_rate(override, "completion");
        tier->cost_cache_read = parse_token_rate(override, "input_cache_read");
        tier->cost_cache_write = parse_token_rate(override, "input_cache_write");
        tier->cost_cache_write_1h = parse_token_rate(override, "input_cache_write_1h");
    }
}

void openrouter_parse_model(const json_t *entry, struct model_info *info)
{
    json_t *context_length = json_object_get(entry, "context_length");
    if (json_is_integer(context_length) && json_integer_value(context_length) > 0)
        info->context = (long)json_integer_value(context_length);

    json_t *top_provider = json_object_get(entry, "top_provider");
    json_t *max_completion = json_is_object(top_provider)
                                 ? json_object_get(top_provider, "max_completion_tokens")
                                 : NULL;
    if (json_is_integer(max_completion) && json_integer_value(max_completion) > 0)
        info->max_output = (long)json_integer_value(max_completion);

    json_t *architecture = json_object_get(entry, "architecture");
    json_t *input_modalities =
        json_is_object(architecture) ? json_object_get(architecture, "input_modalities") : NULL;
    info->image_input = capability_from_array(input_modalities, "image");
    info->tools = capability_from_array(json_object_get(entry, "supported_parameters"), "tools");

    json_t *pricing = json_object_get(entry, "pricing");
    if (json_is_object(pricing)) {
        info->cost_input = parse_token_rate(pricing, "prompt");
        info->cost_output = parse_token_rate(pricing, "completion");
        info->cost_cache_read = parse_token_rate(pricing, "input_cache_read");
        info->cost_cache_write = parse_token_rate(pricing, "input_cache_write");
        info->cost_cache_write_1h = parse_token_rate(pricing, "input_cache_write_1h");
        parse_pricing_tiers(pricing, info);
    }

    openrouter_parse_efforts(entry, &info->efforts);
    info->description = parse_description_lead(entry);
}

void openrouter_parse_efforts(const json_t *entry, struct effort_set *efforts)
{
    json_t *reasoning = json_object_get(entry, "reasoning");
    if (!json_is_object(reasoning)) {
        json_t *parameters = json_object_get(entry, "supported_parameters");
        if (!json_is_array(parameters))
            return;

        /* An accepted reasoning parameter without model-specific metadata leaves categorical
         * levels unknown. */
        for (size_t i = 0; i < json_array_size(parameters); i++) {
            const char *parameter = json_string_value(json_array_get(parameters, i));
            if (parameter &&
                (strcmp(parameter, "reasoning") == 0 || strcmp(parameter, "reasoning_effort") == 0))
                return;
        }
        efforts->known = 1;
        return;
    }

    json_t *levels = json_object_get(reasoning, "supported_efforts");
    if (!json_is_array(levels))
        return;

    efforts->known = 1;
    for (size_t i = 0; i < json_array_size(levels); i++)
        effort_set_add(efforts, json_string_value(json_array_get(levels, i)));
}

void openrouter_parse_model_probe_response(const char *body, const char *model,
                                           struct model_info *info)
{
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return;

    json_t *models = json_object_get(root, "data");
    if (json_is_array(models)) {
        size_t index;
        json_t *entry;
        json_array_foreach(models, index, entry)
        {
            const char *id = json_string_value(json_object_get(entry, "id"));
            if (id && strcmp(id, model) == 0) {
                openrouter_parse_model(entry, info);
                break;
            }
        }
    }
    json_decref(root);
}

static char *encode_query_value(const char *value)
{
    static const char HEX[] = "0123456789ABCDEF";
    struct buf encoded;
    buf_init(&encoded);

    for (const unsigned char *byte = (const unsigned char *)value; *byte; byte++) {
        if ((*byte >= 'A' && *byte <= 'Z') || (*byte >= 'a' && *byte <= 'z') ||
            (*byte >= '0' && *byte <= '9') || *byte == '-' || *byte == '_' || *byte == '.' ||
            *byte == '~') {
            buf_append(&encoded, (const char *)byte, 1);
        } else {
            char escape[3] = {'%', HEX[*byte >> 4], HEX[*byte & 0xf]};
            buf_append(&encoded, escape, sizeof(escape));
        }
    }
    return buf_steal(&encoded);
}

int openrouter_probe_model(struct provider *provider, const char *model, struct model_probe *probe)
{
    (void)provider;
    if (!model || !*model)
        return -1;

    char *encoded_model = encode_query_value(model);
    probe->url = xasprintf(OPENROUTER_MODELS_ENDPOINT "?q=%s", encoded_model);
    free(encoded_model);

    const char *api_key = openrouter_api_key();
    char *authorization = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *fixed[] = {authorization, NULL};
    char **extra_headers = provider_extra_headers("providers.openrouter");
    probe->headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(authorization);
    probe->timeout_s = MODEL_PROBE_TIMEOUT_S;
    probe->parse = openrouter_parse_model_probe_response;
    return 0;
}

#define USAGE_LABEL_WIDTH 11

static void print_key_usage(const json_t *data)
{
    const char *label = json_string_value(json_object_get(data, "label"));
    printf(ANSI_DIM "openrouter");
    /* Default labels contain a masked API key, which should not enter scrollback; a custom
     * label is server text, so keep terminal controls out of it. */
    if (label && *label && strncmp(label, "sk-", 3) != 0) {
        char *safe_label = ctrl_strip_line_dup(label);
        printf(" · %s", safe_label);
        free(safe_label);
    }
    if (json_is_true(json_object_get(data, "is_free_tier")))
        printf(" · free tier");
    printf(ANSI_RESET "\n");

    char amount[32];
    json_t *spent = json_object_get(data, "usage");
    if (json_is_number(spent)) {
        format_cost(amount, sizeof(amount), json_number_value(spent));
        printf("  " ANSI_DIM "%-*s%s" ANSI_RESET "\n", USAGE_LABEL_WIDTH, "spent", amount);
    }

    json_t *limit = json_object_get(data, "limit");
    if (!json_is_number(limit))
        return;

    format_cost(amount, sizeof(amount), json_number_value(limit));
    printf("  " ANSI_DIM "%-*s%s", USAGE_LABEL_WIDTH, "key limit", amount);
    json_t *remaining = json_object_get(data, "limit_remaining");
    if (json_is_number(remaining)) {
        format_cost(amount, sizeof(amount), json_number_value(remaining));
        printf(" · %s remaining", amount);
    }
    printf(ANSI_RESET "\n");
}

static void print_account_credits(const char *body)
{
    if (!body)
        return;

    json_t *root = json_loads(body, 0, NULL);
    json_t *data = root ? json_object_get(root, "data") : NULL;
    json_t *total = json_is_object(data) ? json_object_get(data, "total_credits") : NULL;
    json_t *spent = json_is_object(data) ? json_object_get(data, "total_usage") : NULL;
    /* Zero credits denotes a free or BYOK account, not an exhausted prepaid balance. */
    if (!json_is_number(total) || !json_is_number(spent) || json_number_value(total) <= 0)
        goto out;

    double remaining = json_number_value(total) - json_number_value(spent);
    if (remaining < 0)
        remaining = 0;

    char remaining_text[32], total_text[32];
    format_cost(remaining_text, sizeof(remaining_text), remaining);
    format_cost(total_text, sizeof(total_text), json_number_value(total));
    printf("  " ANSI_DIM "%-*s%s of %s remaining" ANSI_RESET "\n", USAGE_LABEL_WIDTH, "credits",
           remaining_text, total_text);

out:
    json_decref(root);
}

int openrouter_query_usage(struct provider *provider)
{
    (void)provider;
    const char *api_key = openrouter_api_key();
    if (!api_key) {
        ui_error("no OpenRouter API key configured");
        return -1;
    }

    char *authorization = xasprintf("Authorization: Bearer %s", api_key);
    const char *fixed[] = {authorization, "Accept: application/json", NULL};
    char **extra_headers = provider_extra_headers("providers.openrouter");
    char **headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(authorization);
    char *key_body = NULL, *credits_body = NULL;
    json_t *root = NULL;
    int result = -1;
    long status = 0;

    struct busy *busy = busy_begin("fetching usage...");
    int request_result = http_get(OPENROUTER_KEY_ENDPOINT, (const char *const *)headers,
                                  USAGE_TIMEOUT_S, 0, busy_tick, NULL, &key_body, &status);
    if (request_result == 0 && key_body)
        http_get(OPENROUTER_CREDITS_ENDPOINT, (const char *const *)headers, USAGE_TIMEOUT_S, 0,
                 busy_tick, NULL, &credits_body, NULL);
    int cancelled = busy_end(busy);
    string_array_free(headers);

    if (cancelled)
        goto out;
    if (request_result != 0 || !key_body) {
        if (status == 401)
            ui_error("OpenRouter rejected the configured API key (401)");
        else
            ui_error("failed to fetch usage from %s", OPENROUTER_KEY_ENDPOINT);
        goto out;
    }

    json_error_t error;
    root = json_loads(key_body, 0, &error);
    if (!root) {
        ui_error("usage response is not valid JSON: %s", error.text);
        goto out;
    }

    json_t *data = json_object_get(root, "data");
    if (!json_is_object(data)) {
        ui_error("unrecognized usage response shape (no data object)");
        goto out;
    }

    print_key_usage(data);
    print_account_credits(credits_body);
    result = 0;

out:
    json_decref(root);
    free(key_body);
    free(credits_body);
    return result;
}
