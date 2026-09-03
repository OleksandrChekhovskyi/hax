/* SPDX-License-Identifier: MIT */
#include "providers/opencode.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "busy.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/http_provider.h"
#include "providers/usage_render.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "transport/http.h"

#define OPENCODE_USAGE_TIMEOUT_S   30
#define OPENCODE_USAGE_WINDOWS_MAX 8

/* "2026-08-21T21:14:51.969Z" → epoch seconds, or -1 on any other shape. Fractional seconds are
 * ignored. Only UTC is accepted: a nonzero offset would shift the reset time silently. */
static time_t parse_utc_timestamp(const char *text)
{
    int year, month, day, hour, minute, second, parsed_length = 0;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute, &second,
               &parsed_length) != 6)
        return -1;
    if (year < 1970 || month < 1 || month > 12 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 60)
        return -1;
    /* Enforce real calendar days; the day-count formula below would silently normalize an
     * overflow like Feb 31 into the next month. */
    static const int MONTH_DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day < 1 || day > MONTH_DAYS[month - 1] + (month == 2 && leap))
        return -1;
    const char *rest = text + parsed_length;
    if (*rest == '.') {
        if (rest[1] < '0' || rest[1] > '9')
            return -1;
        for (rest++; *rest >= '0' && *rest <= '9'; rest++)
            ;
    }
    if (strcmp(rest, "Z") != 0)
        return -1;

    /* Civil date → days since 1970-01-01 (Hinnant's days_from_civil). The library converters
     * are out of reach: timegm (BSD, standard only since C23) is hidden by -std=c11 plus our
     * feature macros, mktime works in local time, and curl_getdate does not parse ISO 8601. */
    int shifted_year = year - (month <= 2);
    int era = shifted_year / 400;
    int year_of_era = shifted_year - era * 400;
    int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    long long days = (long long)era * 146097 + day_of_era - 719468;
    return (time_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

size_t opencode_usage_parse(json_t *root, struct usage_window *windows, size_t max)
{
    json_t *usage = json_object_get(root, "usage");
    size_t count = 0;
    const char *name;
    json_t *window;
    json_object_foreach(usage, name, window)
    {
        if (count == max)
            break;
        json_t *percent = json_object_get(window, "percent");
        const char *resets_at = json_string_value(json_object_get(window, "resetsAt"));
        if (!json_is_number(percent) || !resets_at)
            continue;
        time_t reset_at = parse_utc_timestamp(resets_at);
        if (reset_at < 0)
            continue;
        const char *status = json_string_value(json_object_get(window, "status"));
        windows[count++] = (struct usage_window){
            .label = name,
            .used_percent = json_number_value(percent),
            .reset_at = reset_at,
            .note = status && strcmp(status, "ok") != 0 ? status : NULL,
        };
    }
    return count;
}

char **opencode_usage_headers(const struct provider *provider)
{
    /* Not http_provider_metadata_headers: those follow the model wire and switch to x-api-key
     * when the gateway is overridden to the Messages dialect, but the usage endpoint accepts
     * only Bearer auth. */
    char *authorization = xasprintf("Authorization: Bearer %s", http_provider_api_key(provider));
    const char *fixed[] = {authorization, "Accept: application/json", NULL};
    char **extra_headers = http_provider_extra_headers(provider);
    char **headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(authorization);
    return headers;
}

int opencode_go_query_usage(struct provider *provider)
{
    if (!http_provider_has_api_key(provider)) {
        ui_error("no OPENCODE_API_KEY configured");
        return -1;
    }

    char *url = xasprintf("%s/usage", http_provider_base_url(provider));
    char **headers = opencode_usage_headers(provider);
    char *body = NULL;
    json_t *root = NULL;
    long status = 0;
    int result = -1;

    struct busy *busy = busy_begin("fetching usage...");
    int request_result = http_get(url, (const char *const *)headers, OPENCODE_USAGE_TIMEOUT_S, 0,
                                  busy_tick, NULL, &body, &status);
    int cancelled = busy_end(busy);
    string_array_free(headers);

    if (cancelled)
        goto out;
    if (request_result != 0 || !body) {
        if (status == 401)
            ui_error("opencode rejected the configured API key (401)");
        else
            ui_error("failed to fetch usage from %s", url);
        goto out;
    }

    json_error_t error;
    root = json_loads(body, 0, &error);
    if (!root) {
        ui_error("usage response is not valid JSON: %s", error.text);
        goto out;
    }

    struct usage_window windows[OPENCODE_USAGE_WINDOWS_MAX];
    size_t n_windows = opencode_usage_parse(root, windows, OPENCODE_USAGE_WINDOWS_MAX);
    if (n_windows == 0) {
        ui_error("unrecognized usage response shape (no usage windows)");
        goto out;
    }

    printf(ANSI_DIM "%s" ANSI_RESET "\n", provider->name);
    for (size_t i = 0; i < n_windows; i++)
        usage_window_print(&windows[i]);
    result = 0;

out:
    json_decref(root);
    free(body);
    free(url);
    return result;
}
