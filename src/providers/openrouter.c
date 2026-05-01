/* SPDX-License-Identifier: MIT */
#include "openrouter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "openai.h"
#include "util.h"

/* Best-effort, silent on failure: a 5s budget is generous for OpenRouter's
 * model catalog response (~hundreds of entries, ~hundreds of KB) on a
 * normal connection. Falling back to no probe just hides the percentage
 * display, which the user can also fill manually with HAX_CONTEXT_LIMIT. */
#define PROBE_TIMEOUT_S 5

static void probe_context_limit(const char *api_key)
{
    const char *cur = getenv("HAX_CONTEXT_LIMIT");
    if (cur && *cur)
        return;
    const char *model = getenv("HAX_MODEL");
    if (!model || !*model)
        return; /* nothing to look up — agent will surface the missing-model error */

    char *url = xstrdup("https://openrouter.ai/api/v1/models");
    char *auth = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *headers[] = {auth, NULL};
    char *body = NULL;
    if (http_get(url, auth ? headers : NULL, PROBE_TIMEOUT_S, &body) == 0) {
        json_t *root = json_loads(body, 0, NULL);
        if (root) {
            json_t *data = json_object_get(root, "data");
            if (json_is_array(data)) {
                size_t i;
                json_t *entry;
                json_array_foreach(data, i, entry)
                {
                    json_t *id = json_object_get(entry, "id");
                    if (!json_is_string(id) || strcmp(json_string_value(id), model) != 0)
                        continue;
                    json_t *ctx = json_object_get(entry, "context_length");
                    if (json_is_integer(ctx) && json_integer_value(ctx) > 0) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%lld", (long long)json_integer_value(ctx));
                        setenv("HAX_CONTEXT_LIMIT", buf, 1);
                    }
                    break;
                }
            }
            json_decref(root);
        }
    }
    free(body);
    free(auth);
    free(url);
}

struct provider *openrouter_provider_new(void)
{
    /* Locked to openrouter.ai — HAX_OPENAI_BASE_URL is rejected so the
     * OPENROUTER_API_KEY fallback (and the public X-Title attribution
     * header) can never leak to an unrelated host. Custom OpenAI-compat
     * endpoints belong on HAX_PROVIDER=openai-compatible. */
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    if (base_env && *base_env) {
        fprintf(stderr, "hax: HAX_OPENAI_BASE_URL is not honored by HAX_PROVIDER=openrouter "
                        "(this preset is locked to openrouter.ai)\n"
                        "hax: use HAX_PROVIDER=openai-compatible to point at a custom endpoint\n");
        return NULL;
    }
    const char *key = getenv("HAX_OPENAI_API_KEY");
    if (!key || !*key)
        key = getenv("OPENROUTER_API_KEY");
    probe_context_limit((key && *key) ? key : NULL);

    const char *title = getenv("HAX_OPENROUTER_TITLE");
    if (!title || !*title)
        title = "hax";
    const char *referer = getenv("HAX_OPENROUTER_REFERER");

    /* OpenRouter uses these for attribution on its public leaderboards and
     * usage dashboards. Both are optional from OpenRouter's side; we always
     * send X-Title (so traffic is recognizable) but only send HTTP-Referer
     * when the user explicitly opts in. */
    char *title_hdr = xasprintf("X-Title: %s", title);
    char *referer_hdr = (referer && *referer) ? xasprintf("HTTP-Referer: %s", referer) : NULL;

    const char *headers[3];
    size_t i = 0;
    headers[i++] = title_hdr;
    if (referer_hdr)
        headers[i++] = referer_hdr;
    headers[i] = NULL;

    struct openai_preset preset = {
        .display_name = "openrouter",
        .default_base_url = "https://openrouter.ai/api/v1",
        .api_key_env = "OPENROUTER_API_KEY",
        .send_cache_key_default = 1,
        .extra_headers = headers,
    };
    /* Constructor copies headers internally, so the local strings can be
     * freed once it returns. */
    struct provider *p = openai_provider_new_preset(&preset);
    free(title_hdr);
    free(referer_hdr);
    return p;
}

const struct provider_factory PROVIDER_OPENROUTER = {
    .name = "openrouter",
    .new = openrouter_provider_new,
};
