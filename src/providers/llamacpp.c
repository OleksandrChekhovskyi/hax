/* SPDX-License-Identifier: MIT */
#include "llamacpp.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "openai.h"
#include "probe.h"
#include "util.h"
#include "transport/http.h"

/* Model probe runs synchronously at startup because HAX_MODEL must be
 * resolved before the first chat request body is built. The context-limit
 * probe is async (see spawn_context_probe) so a slow /props doesn't delay
 * the first prompt. Both stay short so a missing or slow server fails
 * cleanly. */
#define MODEL_PROBE_TIMEOUT_S 2
#define CTX_PROBE_TIMEOUT_S   5

/* Build a sibling URL with the same scheme/host/port as `base` but a
 * different path. Used to reach llama-server's `/props` (rooted, not under
 * `/v1`). Returns a heap-owned string or NULL on failure. */
static char *swap_path(const char *base, const char *new_path)
{
    CURLU *u = curl_url();
    if (!u)
        return NULL;
    char *out = NULL;
    if (curl_url_set(u, CURLUPART_URL, base, 0) == CURLUE_OK &&
        curl_url_set(u, CURLUPART_PATH, new_path, 0) == CURLUE_OK) {
        char *built = NULL;
        if (curl_url_get(u, CURLUPART_URL, &built, 0) == CURLUE_OK) {
            out = xstrdup(built);
            curl_free(built);
        }
    }
    curl_url_cleanup(u);
    return out;
}

/* Returns 0 on success (HAX_MODEL was already set, or auto-fill worked).
 * Returns -1 when HAX_MODEL was unset AND we failed to derive one from
 * /v1/models — a strong signal the server is unreachable, which the caller
 * surfaces as a hard error so the user sees the real cause instead of a
 * downstream "HAX_MODEL is required" message. */
static int probe_model(const char *base_url, const char *api_key)
{
    const char *cur = getenv("HAX_MODEL");
    if (cur && *cur)
        return 0;
    char *url = xasprintf("%s/models", base_url);
    char *auth = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *headers[] = {auth, NULL};
    char *body = NULL;
    int ok = 0;
    if (http_get(url, auth ? headers : NULL, MODEL_PROBE_TIMEOUT_S, NULL, NULL, &body) == 0) {
        json_t *root = json_loads(body, 0, NULL);
        if (root) {
            json_t *data = json_object_get(root, "data");
            if (json_is_array(data) && json_array_size(data) > 0) {
                json_t *id = json_object_get(json_array_get(data, 0), "id");
                if (json_is_string(id)) {
                    setenv("HAX_MODEL", json_string_value(id), 1);
                    ok = 1;
                }
            }
            json_decref(root);
        }
    }
    free(body);
    free(auth);
    free(url);
    return ok ? 0 : -1;
}

/* /props returns the live n_ctx the server was started with — the only
 * cross-version-stable way to learn the context window from llama-server.
 * `user` is unused (single global value, no key to match). */
static long extract_llamacpp_n_ctx(const char *body, void *user)
{
    (void)user;
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return 0;
    long out = 0;
    json_t *settings = json_object_get(root, "default_generation_settings");
    json_t *n_ctx = settings ? json_object_get(settings, "n_ctx") : NULL;
    if (json_is_integer(n_ctx) && json_integer_value(n_ctx) > 0)
        out = (long)json_integer_value(n_ctx);
    json_decref(root);
    return out;
}

static void spawn_context_probe(struct provider *p, const char *base_url, const char *api_key)
{
    /* User-supplied HAX_CONTEXT_LIMIT wins; nothing for the probe to add. */
    const char *cur = getenv("HAX_CONTEXT_LIMIT");
    if (cur && *cur)
        return;
    char *url = swap_path(base_url, "/props");
    if (!url)
        return;

    struct probe_args *a = xcalloc(1, sizeof(*a));
    a->url = url;
    if (api_key && *api_key) {
        a->headers = xcalloc(2, sizeof(*a->headers));
        a->headers[0] = xasprintf("Authorization: Bearer %s", api_key);
        a->headers[1] = NULL;
    }
    a->timeout_s = CTX_PROBE_TIMEOUT_S;
    a->extract = extract_llamacpp_n_ctx;
    a->target = &p->context_limit;
    openai_attach_probe(p, probe_context_limit_spawn(a));
}

struct provider *llamacpp_provider_new(void)
{
    const char *port_env = getenv("HAX_LLAMACPP_PORT");
    const char *port = (port_env && *port_env) ? port_env : "8080";
    char *default_url = xasprintf("http://127.0.0.1:%s/v1", port);

    /* Probe whichever URL the openai constructor will actually use, so a
     * user-supplied HAX_OPENAI_BASE_URL still benefits from auto-discovery.
     * Normalize the trailing slash up-front so the probe and the eventual
     * stream() target produce identical paths. */
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    char *resolved = dup_trim_trailing_slash((base_env && *base_env) ? base_env : default_url);
    /* llama-server can be started with --api-key, in which case HAX_OPENAI_API_KEY
     * carries the matching token. Forward it to the probes too — otherwise an
     * authenticated server returns 401 on /v1/models and provider construction
     * fails even though the eventual chat request would have been authorized. */
    const char *key = getenv("HAX_OPENAI_API_KEY");
    if (key && !*key)
        key = NULL;
    if (probe_model(resolved, key) != 0) {
        fprintf(stderr,
                "hax: llama.cpp: failed to auto-discover model from %s/models\n"
                "hax: is llama-server running? "
                "(set HAX_MODEL to skip probing, or adjust HAX_LLAMACPP_PORT / "
                "HAX_OPENAI_BASE_URL)\n",
                resolved);
        free(resolved);
        free(default_url);
        return NULL;
    }

    struct openai_preset preset = {
        .display_name = "llama.cpp",
        .default_base_url = default_url,
        .send_cache_key_default = 0,
        /* llama-server attaches `prompt_progress` to each prefill chunk
         * when this is set — the only provider we target today that
         * exposes server-side progress. */
        .emit_progress = 1,
    };
    struct provider *p = openai_provider_new_preset(&preset);
    /* Context-limit probe runs in the background: an older llama-server
     * without /props, or a proxy that doesn't expose it, just means the
     * percentage display is hidden — not a reason to refuse to start
     * (or to delay the first prompt). */
    if (p)
        spawn_context_probe(p, resolved, key);
    free(resolved);
    free(default_url);
    return p;
}

const struct provider_factory PROVIDER_LLAMACPP = {
    .name = "llama.cpp",
    .new = llamacpp_provider_new,
};
