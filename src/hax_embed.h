/* SPDX-License-Identifier: MIT */
#ifndef HAX_HAX_EMBED_H
#define HAX_HAX_EMBED_H

#include <stddef.h>

#include "util.h"

/* Lifecycle for callers that are not main(): language bindings, test harnesses, and any host
 * process that owns its own locale, libcurl, and exit handling. hax_init() must complete before
 * any other hax call, and returns -1 when called twice.
 *
 * Everything hax_init() sets up is process-wide: one embedded agent per process. */

struct hax_embed_options {
    /* Nonzero calls setlocale(LC_CTYPE) and publishes LC_CTYPE to the environment. Embedders
     * leave this zero: setenv() races any thread already reading the environment, and the host's
     * locale is not ours to change. Multibyte decoding still works from the inherited locale. */
    int own_locale;
    /* Nonzero calls curl_global_init() here and curl_global_cleanup() in hax_shutdown(). Zero
     * when the host initialized libcurl already, which is not safe to do twice. */
    int own_curl_global;
    /* Nonzero registers the atexit() handlers that remove temporary files and close the trace
     * log. Zero requires hax_shutdown(): an atexit handler in a module that may be unloaded runs
     * against unmapped code. */
    int own_atexit;
    /* NULL keeps diagnostics on stderr. */
    hax_diag_fn diag;
    void *diag_user;
};

/* Returns 0, or -1 after emitting a diagnostic. */
int hax_init(const struct hax_embed_options *options);

/* Release what hax_init() acquired. Destroy every provider first: providers join background work
 * that global libcurl teardown must outlive. Idempotent, and a no-op when hax_init() failed. */
void hax_shutdown(void);

struct provider;

/* Construct a provider by registry id, or by the configured `provider` setting when `name` is
 * NULL or empty. Returns NULL after emitting a diagnostic. Keeping construction here spares a
 * binding from mirroring struct provider and struct provider_def. */
struct provider *hax_provider_new(const char *name);

/* Destroy a provider obtained from hax_provider_new(). NULL-safe, and required before
 * hax_shutdown(). */
void hax_provider_destroy(struct provider *provider);

/* Sizes of the structs a foreign-function binding builds and passes across. A binding that
 * compiles against these headers already agrees on their layout; this catches the other case, a
 * libhax swapped underneath an extension built earlier, where a silent size change would corrupt
 * memory. Compare it at load time and refuse to run on a mismatch.
 *
 * Extend by appending, and bump `version` whenever an existing field changes meaning. */
struct hax_abi {
    unsigned version;
    size_t sizeof_item;
    size_t sizeof_agent_session;
    size_t sizeof_agent_loop_params;
    size_t sizeof_agent_loop_result;
    size_t sizeof_agent_loop_hooks;
};

/* Borrowed; static storage. */
const struct hax_abi *hax_abi(void);

#define HAX_ABI_VERSION 1u

#endif /* HAX_HAX_EMBED_H */
