/* SPDX-License-Identifier: MIT */
#include "ollama.h"

#include <stdlib.h>

#include "config.h"
#include "openai.h"
#include "util.h"

struct provider *ollama_provider_new(void)
{
    /* No model is enforced here. Ollama has no server-side notion of a
     * "current" model — chat requests must name one (the endpoint 400s on
     * an empty model field) — but constructing the provider doesn't need it:
     * agent_session_init / oneshot enforce "HAX_MODEL is required (no
     * default)" at startup, and the runtime /model picker discovers the
     * installed models from /v1/models and sets one. Keeping construction
     * model-free is what lets /provider switch *to* ollama and then list its
     * models (the picker can't run before the provider exists).
     *
     * Unlike llama.cpp, ollama is not context-probed: the only metadata it
     * exposes before a model is loaded (/api/show) is the model's *training*
     * context length, not the runtime window — which is OLLAMA_CONTEXT_LENGTH
     * (default 4096) and only readable from /api/ps once the model is loaded
     * by the first request. A startup probe would therefore report the
     * training maximum, badly over-stating headroom and masking the very
     * truncation it's meant to warn about. So leave the %-of-context display
     * to HAX_CONTEXT_LIMIT (set it to your OLLAMA_CONTEXT_LENGTH), and let the
     * length-truncation hint below explain the failure if the window fills.
     *
     * The "11434" default lives in the config registry, so it's defined in
     * one place. */
    char *default_url = xasprintf("http://127.0.0.1:%s/v1", config_str_nonempty("ollama.port"));

    struct openai_preset preset = {
        .display_name = "ollama",
        .default_base_url = default_url,
        /* Local server: prefix caching isn't advertised, and ollama
         * ignores unknown request fields, so leave the key off by
         * default and let the user opt in via HAX_OPENAI_SEND_CACHE_KEY
         * if their setup honors it. */
        .send_cache_key_default = 0,
        /* ollama caps the runtime context at OLLAMA_CONTEXT_LENGTH (4096 by
         * default) and ignores a per-request num_ctx on its OpenAI endpoint,
         * so hax can't widen it — a prompt near that size truncates the reply
         * to "length" after a token or two. Point the user at the only real
         * fix (a bigger server-side context) instead of a bare "length". */
        .length_hint = "ollama's context window may be too small for the prompt — "
                       "restart `ollama serve` with a larger OLLAMA_CONTEXT_LENGTH "
                       "(e.g. 16384), or raise num_ctx on the model",
    };
    struct provider *p = openai_provider_new_preset(&preset);
    free(default_url);
    return p;
}

/* Availability is "is the ollama daemon up": a bounded GET on its
 * OpenAI-compat /v1/models. Resolves the base URL as the constructor does. */
static int ollama_available(const char **reason)
{
    char *default_url = xasprintf("http://127.0.0.1:%s/v1", config_str_nonempty("ollama.port"));
    const char *base_env = config_str("openai.base_url");
    char *resolved = dup_trim_trailing_slash((base_env && *base_env) ? base_env : default_url);
    const char *key = config_str("openai.api_key");
    int ok = openai_base_url_reachable(resolved, (key && *key) ? key : NULL, reason);
    free(resolved);
    free(default_url);
    return ok;
}

const struct provider_factory PROVIDER_OLLAMA = {
    .name = "ollama",
    .new = ollama_provider_new,
    .available = ollama_available,
};
