/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSPORT_CA_H
#define HAX_TRANSPORT_CA_H

#include <curl/curl.h>

/* Runtime discovery of the system TLS root certificate store, for binaries running on a
 * different distribution than libcurl's build baked its store path for (static or relocated
 * builds). */

/* Resolve the store once, from the main thread, after curl_global_init() and before the first
 * request. Honors the standard CURL_CA_BUNDLE / SSL_CERT_FILE / SSL_CERT_DIR variables, which
 * libcurl itself does not read. Without this call, ca_apply() and ca_verify_hint() are no-ops. */
void ca_init(void);

/* Point `curl` at the resolved store: environment overrides govern origin verification only,
 * while HTTPS proxies always verify against the probed system store. A no-op when libcurl's
 * own default is in effect. */
void ca_apply(CURL *curl);

/* Advice to append to `code`'s error message when it can stem from the missing store, or NULL
 * when the store is not the problem. */
const char *ca_verify_hint(CURLcode code);

/* Emit ca_verify_hint()'s advice through hax_warn(), once per process; for transports whose
 * error bodies never reach the user. */
void ca_warn_verify_failure(CURLcode code);

enum ca_store_source {
    CA_STORE_MISSING = -1, /* nothing found anywhere */
    CA_STORE_DEFAULT,      /* libcurl's own default or native OS trust; nothing to set */
    CA_STORE_ENV,          /* *file / *dir from environment overrides */
    CA_STORE_PROBED,       /* *file / *dir probed from the filesystem */
};

/* Resolution core, exposed for tests. `root` prefixes every probed path ("" probes the real
 * filesystem); environment values pass through unprobed. `ssl_version` and the NULL-terminated
 * `features` describe the running libcurl. `*file` / `*dir` receive malloc'd CURLOPT_CAINFO /
 * CURLOPT_CAPATH values, or NULL to leave the option alone. */
enum ca_store_source ca_resolve(const char *root, const char *env_bundle, const char *env_file,
                                const char *env_dir, const char *ssl_version,
                                const char *const *features, const char *curl_file,
                                const char *curl_dir, char **file, char **dir);

#endif /* HAX_TRANSPORT_CA_H */
