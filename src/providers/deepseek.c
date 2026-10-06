/* SPDX-License-Identifier: MIT */
#include "providers/deepseek.h"

#include <jansson.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "effort.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/usage_fetch.h"
#include "providers/usage_render.h"
#include "render/ctrl_strip.h"
#include "terminal/ui.h"

#define DEEPSEEK_BALANCES_MAX 4

static long positive_integer(const json_t *entry, const char *key)
{
    json_t *value = json_object_get(entry, key);
    return json_is_integer(value) && json_integer_value(value) > 0 ? (long)json_integer_value(value)
                                                                   : 0;
}

void deepseek_parse_model(const json_t *entry, struct model_info *info)
{
    info->context = positive_integer(entry, "context_window");
    info->max_output = positive_integer(entry, "max_output_tokens");
    info->image_input = provider_cap_listed(json_object_get(entry, "input_modalities"), "image");

    json_t *levels = json_object_get(json_object_get(entry, "effort"), "supported_levels");
    if (json_is_array(levels)) {
        /* The levels grade thinking; `none` turns it off and is accepted though never listed. */
        effort_set_add(&info->efforts, "none");
        size_t index;
        json_t *level;
        json_array_foreach(levels, index, level)
            effort_set_add(&info->efforts, json_string_value(level));
    }

    /* The id may be an alias, such as deepseek-flash; the name says which version serves it. */
    const char *name = json_string_value(json_object_get(entry, "name"));
    if (name && *name)
        info->description = xstrdup(name);
}

/* Amounts arrive as decimal strings; one that does not parse marks a malformed entry. The
 * leading-character check stops strtod from skipping whitespace, so a validated amount holds no
 * control characters and prints verbatim. */
static int parse_amount(const char *text, double *out)
{
    if (!text || !((*text >= '0' && *text <= '9') || *text == '-'))
        return 0;
    char *end = NULL;
    double amount = strtod(text, &end);
    if (*end != '\0' || !isfinite(amount))
        return 0;
    *out = amount;
    return 1;
}

size_t deepseek_balance_parse(json_t *root, int *available, struct deepseek_balance *balances,
                              size_t max)
{
    *available = !json_is_false(json_object_get(root, "is_available"));
    size_t count = 0;
    size_t index;
    json_t *info;
    json_array_foreach(json_object_get(root, "balance_infos"), index, info)
    {
        if (count == max)
            break;
        const char *currency = json_string_value(json_object_get(info, "currency"));
        const char *total = json_string_value(json_object_get(info, "total_balance"));
        const char *granted = json_string_value(json_object_get(info, "granted_balance"));
        double amount;
        if (!currency || !*currency || !parse_amount(total, &amount))
            continue;
        balances[count++] = (struct deepseek_balance){
            .currency = currency,
            .total = total,
            .granted = parse_amount(granted, &amount) && amount > 0 ? granted : NULL,
        };
    }
    return count;
}

/* "$2.00" and "¥2.00" for the currencies DeepSeek bills in, "2.00 XYZ" otherwise. Owned. */
static char *format_amount(const char *amount, const char *currency)
{
    if (strcmp(currency, "USD") == 0)
        return xasprintf("$%s", amount);
    if (strcmp(currency, "CNY") == 0)
        return xasprintf("¥%s", amount);
    char *code = ctrl_strip_line_dup(currency);
    char *text = xasprintf("%s %s", amount, code);
    free(code);
    return text;
}

char *deepseek_balance_format(const struct deepseek_balance *balance)
{
    char *total = format_amount(balance->total, balance->currency);
    if (!balance->granted)
        return total;
    char *granted = format_amount(balance->granted, balance->currency);
    char *text = xasprintf("%s · %s granted", total, granted);
    free(granted);
    free(total);
    return text;
}

int deepseek_query_usage(struct provider *provider)
{
    if (!http_provider_has_api_key(provider)) {
        ui_error("no DEEPSEEK_API_KEY configured");
        return -1;
    }

    char *url = xasprintf("%s/user/balance", http_provider_base_url(provider));
    char **headers = http_provider_metadata_headers(provider);
    json_t *root = usage_fetch_json(url, (const char *const *)headers, "DeepSeek");
    string_array_free(headers);
    free(url);
    if (!root)
        return -1;

    int available;
    struct deepseek_balance balances[DEEPSEEK_BALANCES_MAX];
    size_t n_balances = deepseek_balance_parse(root, &available, balances, DEEPSEEK_BALANCES_MAX);
    if (n_balances == 0) {
        ui_error("unrecognized balance response shape (no balances)");
        json_decref(root);
        return -1;
    }

    const char *details[] = {available ? NULL : "balance too low for API calls"};
    usage_heading_print(provider->name, details, 1);
    for (size_t i = 0; i < n_balances; i++) {
        char *text = deepseek_balance_format(&balances[i]);
        usage_value_print("balance", "%s", text);
        free(text);
    }
    json_decref(root);
    return 0;
}
