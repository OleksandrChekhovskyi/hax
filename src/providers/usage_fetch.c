/* SPDX-License-Identifier: MIT */
#include "providers/usage_fetch.h"

#include <jansson.h>
#include <stdlib.h>

#include "busy.h"
#include "terminal/ui.h"
#include "transport/http.h"

#define USAGE_TIMEOUT_S 30

json_t *usage_fetch_json(const char *url, const char *const *headers, const char *provider_label)
{
    char *body = NULL;
    long status = 0;
    struct busy *busy = busy_begin("fetching usage...");
    int request_result =
        http_get(url, headers, USAGE_TIMEOUT_S, 0, busy_tick, NULL, &body, &status);
    int cancelled = busy_end(busy);

    json_t *root = NULL;
    if (cancelled)
        goto out;
    if (request_result != 0 || !body) {
        if (status == 401)
            ui_error("%s rejected the configured API key (401)", provider_label);
        else
            ui_error("failed to fetch usage from %s", url);
        goto out;
    }

    json_error_t error;
    root = json_loads(body, 0, &error);
    if (!root)
        ui_error("usage response is not valid JSON: %s", error.text);

out:
    free(body);
    return root;
}
