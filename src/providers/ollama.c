/* SPDX-License-Identifier: MIT */
#include "ollama.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "openai.h"
#include "probe.h"
#include "util.h"

/* Context-limit probe runs in the background so a slow /api/show
 * doesn't delay the first prompt. Stay short so a missing or slow
 * server fails cleanly. */
#define CTX_PROBE_TIMEOUT_S 5

/* Build a sibling URL with the same scheme/host/port as `base` but a
 * different path. Used to reach ollama's `/api/show` which sits at the
 * root rather than under `/v1`. Returns a heap-owned string or NULL on
 * failure. */
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

/* /api/show returns model_info as a flat map of gguf KV pairs. The
 * per-architecture context size lives at "<arch>.context_length" — e.g.
 * "llama.context_length", "qwen2.context_length", "gemma2.context_length"
 * — where <arch> matches model_info["general.architecture"]. Look that
 * up first and fall back to scanning for any `*.context_length` key,
 * which keeps us working on safetensors models and any future
 * architecture whose name we don't yet recognize. `user` is unused — the
 * URL/body already targets one model, so there's no slug to match
 * against. */
static long extract_ollama_context(const char *body, void *user)
{
    (void)user;
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return 0;
    long out = 0;
    json_t *info = json_object_get(root, "model_info");
    if (json_is_object(info)) {
        json_t *arch = json_object_get(info, "general.architecture");
        if (json_is_string(arch)) {
            char *key = xasprintf("%s.context_length", json_string_value(arch));
            json_t *v = json_object_get(info, key);
            free(key);
            if (json_is_integer(v) && json_integer_value(v) > 0)
                out = (long)json_integer_value(v);
        }
        if (out == 0) {
            const char *k;
            json_t *v;
            json_object_foreach(info, k, v)
            {
                size_t klen = strlen(k);
                static const char suffix[] = ".context_length";
                size_t slen = sizeof(suffix) - 1;
                if (klen > slen && strcmp(k + klen - slen, suffix) == 0 && json_is_integer(v) &&
                    json_integer_value(v) > 0) {
                    out = (long)json_integer_value(v);
                    break;
                }
            }
        }
    }
    json_decref(root);
    return out;
}

static void spawn_context_probe(struct provider *p, const char *base_url, const char *api_key)
{
    /* User-supplied HAX_CONTEXT_LIMIT wins; nothing for the probe to add. */
    const char *cur = getenv("HAX_CONTEXT_LIMIT");
    if (cur && *cur)
        return;
    const char *model = getenv("HAX_MODEL");
    if (!model || !*model)
        return; /* nothing to look up — caller will surface the missing-model error */
    char *url = swap_path(base_url, "/api/show");
    if (!url)
        return;

    struct probe_args *a = xcalloc(1, sizeof(*a));
    a->url = url;
    /* json_dumps escapes the model name correctly even when it contains
     * characters that would be problematic raw (quotes, backslashes). */
    json_t *req = json_pack("{s:s}", "model", model);
    a->body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    if (api_key && *api_key) {
        a->headers = xcalloc(2, sizeof(*a->headers));
        a->headers[0] = xasprintf("Authorization: Bearer %s", api_key);
        a->headers[1] = NULL;
    }
    a->timeout_s = CTX_PROBE_TIMEOUT_S;
    a->extract = extract_ollama_context;
    a->target = &p->context_limit;
    openai_attach_probe(p, probe_context_limit_spawn(a));
}

struct provider *ollama_provider_new(void)
{
    /* Require HAX_MODEL up front rather than trying to auto-fill. Ollama
     * has no server-side notion of a "current" model: chat requests
     * must name one (the endpoint 400s on an empty model field), and
     * neither /api/ps (currently-loaded, decays after OLLAMA_KEEP_ALIVE)
     * nor /v1/models (pulled-model catalog, no preference order) gives
     * a reliable signal of which one the user wants. Any heuristic we
     * picked would surprise some workflow — better to ask once. */
    const char *model = getenv("HAX_MODEL");
    if (!model || !*model) {
        fprintf(stderr, "hax: ollama: HAX_MODEL is required "
                        "(run `ollama list` to see installed models)\n");
        return NULL;
    }

    const char *port_env = getenv("HAX_OLLAMA_PORT");
    const char *port = (port_env && *port_env) ? port_env : "11434";
    char *default_url = xasprintf("http://127.0.0.1:%s/v1", port);

    /* Resolve the URL the openai constructor will actually use so the
     * context probe targets the same host. Normalize the trailing slash
     * up-front so the probe and the eventual stream() target produce
     * identical paths. */
    const char *base_env = getenv("HAX_OPENAI_BASE_URL");
    char *resolved = dup_trim_trailing_slash((base_env && *base_env) ? base_env : default_url);
    /* Ollama doesn't authenticate by default, but a reverse proxy in
     * front of it might — forward HAX_OPENAI_API_KEY to the probe so an
     * authenticated front-end doesn't 401 us at /api/show. */
    const char *key = getenv("HAX_OPENAI_API_KEY");
    if (key && !*key)
        key = NULL;

    struct openai_preset preset = {
        .display_name = "ollama",
        .default_base_url = default_url,
        /* Local server: prefix caching isn't advertised, and ollama
         * ignores unknown request fields, so leave the key off by
         * default and let the user opt in via HAX_OPENAI_SEND_CACHE_KEY
         * if their setup honors it. */
        .send_cache_key_default = 0,
    };
    struct provider *p = openai_provider_new_preset(&preset);
    /* Context-limit probe runs in the background: an ollama version
     * without /api/show, or a proxy that doesn't expose it, just means
     * the percentage display is hidden — not a reason to refuse to
     * start (or to delay the first prompt). */
    if (p)
        spawn_context_probe(p, resolved, key);
    free(resolved);
    free(default_url);
    return p;
}

const struct provider_factory PROVIDER_OLLAMA = {
    .name = "ollama",
    .new = ollama_provider_new,
};
