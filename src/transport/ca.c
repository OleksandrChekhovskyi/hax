/* SPDX-License-Identifier: MIT */
#include "transport/ca.h"

#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <curl/curlver.h>
#include <curl/typecheck-gcc.h>

#include "diag.h"
#include "util.h"

/* The well-known locations curl's configure, Go, and rust-native-certs also probe, minus
 * historic-only paths. */
static const char *const BUNDLE_PATHS[] = {
    "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Arch, Alpine, Gentoo */
    "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL */
    "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* RHEL 7+ consolidated trust store */
    "/usr/local/share/certs/ca-root-nss.crt",            /* FreeBSD */
    "/etc/ssl/cert.pem",                                 /* Alpine, FreeBSD, macOS */
};

static const char *const CERT_DIRS[] = {
    "/etc/ssl/certs",     /* Debian and derivatives */
    "/etc/pki/tls/certs", /* Fedora, RHEL */
};

static char *store_file;
static char *store_dir;
/* The probed system store for HTTPS-proxy verification; equals the origin store unless an
 * environment override claimed the origin. */
static char *proxy_file;
static char *proxy_dir;
static int store_missing;

static char *probe_file(const char *root, const char *path)
{
    if (!path)
        return NULL;

    char *full = xasprintf("%s%s", root, path);
    if (access(full, R_OK) != 0) {
        free(full);
        return NULL;
    }
    return full;
}

static int hashed_cert_name(const char *name)
{
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)name[i]))
            return 0;
    return strcmp(name + 8, ".0") == 0;
}

enum capath_rule {
    CAPATH_NONE, /* the backend ignores CURLOPT_CAPATH */
    CAPATH_HASHED,
    CAPATH_PLAIN,
};

/* A usable CAPATH is never empty; whether its entries must be hashed depends on the backend. */
static char *probe_dir(const char *root, const char *path, enum capath_rule rule)
{
    if (rule == CAPATH_NONE)
        return NULL;

    char *full = probe_file(root, path);
    if (!full)
        return NULL;

    DIR *dir = opendir(full);
    if (!dir) {
        free(full);
        return NULL;
    }
    int found = 0;
    for (struct dirent *entry; !found && (entry = readdir(dir));) {
        if (entry->d_name[0] == '.')
            continue;
        found = rule == CAPATH_PLAIN || hashed_cert_name(entry->d_name);
    }
    closedir(dir);

    if (!found) {
        free(full);
        return NULL;
    }
    return full;
}

/* A MultiSSL string wraps inactive backends in parentheses; only the active one decides. */
static const char *active_backend(const char *ssl_version)
{
    const char *token = ssl_version;

    while (token && *token) {
        if (*token == ' ') {
            token++;
        } else if (*token == '(') {
            token = strchr(token, ')');
            token = token ? token + 1 : NULL;
        } else {
            return token;
        }
    }
    return NULL;
}

static int backend_is(const char *backend, const char *name)
{
    return backend && strncmp(backend, name, strlen(name)) == 0;
}

/* Pointing an OS-trust backend at a bundle file replaces Keychain / Windows trust instead of
 * restoring it. The AppleSecTrust / NativeCA features (curl >= 8.17) apply to any TLS library. */
static int native_trust(const char *ssl_version, const char *const *features)
{
    for (; features && *features; features++)
        if (strcmp(*features, "AppleSecTrust") == 0 || strcmp(*features, "NativeCA") == 0)
            return 1;

    const char *backend = active_backend(ssl_version);
    return backend_is(backend, "SecureTransport") || backend_is(backend, "Schannel");
}

/* Per SSLSUPP_CA_PATH: OpenSSL and its forks require the hashed layout, GnuTLS / mbedTLS /
 * wolfSSL load plain certificate files, and every other backend ignores the option. */
static enum capath_rule backend_capath_rule(const char *ssl_version)
{
    const char *backend = active_backend(ssl_version);

    if (backend_is(backend, "OpenSSL") || backend_is(backend, "LibreSSL") ||
        backend_is(backend, "BoringSSL") || backend_is(backend, "quictls") ||
        backend_is(backend, "AWS-LC"))
        return CAPATH_HASHED;
    if (backend_is(backend, "GnuTLS") || backend_is(backend, "mbedTLS") ||
        backend_is(backend, "wolfSSL"))
        return CAPATH_PLAIN;
    return CAPATH_NONE;
}

enum ca_store_source ca_resolve(const char *root, const char *env_bundle, const char *env_file,
                                const char *env_dir, const char *ssl_version,
                                const char *const *features, const char *curl_file,
                                const char *curl_dir, char **file, char **dir)
{
    *file = NULL;
    *dir = NULL;

    /* A bundle is a complete trust statement: curl also ignores both SSL_CERT variables when
     * CURL_CA_BUNDLE is set. */
    if (env_bundle && *env_bundle) {
        *file = xstrdup(env_bundle);
        return CA_STORE_ENV;
    }
    if (env_file && *env_file)
        *file = xstrdup(env_file);
    if (env_dir && *env_dir)
        *dir = xstrdup(env_dir);
    if (*file || *dir)
        return CA_STORE_ENV;

    if (native_trust(ssl_version, features))
        return CA_STORE_DEFAULT;
    enum capath_rule rule = backend_capath_rule(ssl_version);
    char *found = probe_file(root, curl_file);
    if (!found)
        found = probe_dir(root, curl_dir, rule);
    if (found) {
        free(found);
        return CA_STORE_DEFAULT;
    }

    for (size_t i = 0; !*file && i < sizeof(BUNDLE_PATHS) / sizeof(BUNDLE_PATHS[0]); i++)
        *file = probe_file(root, BUNDLE_PATHS[i]);
    for (size_t i = 0; !*dir && i < sizeof(CERT_DIRS) / sizeof(CERT_DIRS[0]); i++)
        *dir = probe_dir(root, CERT_DIRS[i], rule);
    return *file || *dir ? CA_STORE_PROBED : CA_STORE_MISSING;
}

static void warn_capath_ignored(void)
{
    static atomic_int warned;

    if (atomic_exchange(&warned, 1))
        return;
    hax_warn("SSL_CERT_DIR is ignored: this libcurl's TLS backend does not support"
             " CA directories");
}

void ca_init(void)
{
/* cainfo/capath joined curl_version_info_data in 7.70.0 (CURLVERSION_SEVENTH). */
#if CURL_AT_LEAST_VERSION(7, 70, 0)
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);

    /* Runtimes too old to report their store are distribution builds whose defaults match the
     * system; leave them alone. */
    if (info->age < CURLVERSION_SEVENTH)
        return;
    const char *const *features = NULL;
#if CURL_AT_LEAST_VERSION(7, 87, 0)
    if (info->age >= CURLVERSION_ELEVENTH)
        features = info->feature_names;
#endif
    enum ca_store_source source = ca_resolve("", getenv("CURL_CA_BUNDLE"), getenv("SSL_CERT_FILE"),
                                             getenv("SSL_CERT_DIR"), info->ssl_version, features,
                                             info->cainfo, info->capath, &store_file, &store_dir);
    store_missing = source == CA_STORE_MISSING;
    if (source == CA_STORE_PROBED) {
        proxy_file = store_file ? xstrdup(store_file) : NULL;
        proxy_dir = store_dir ? xstrdup(store_dir) : NULL;
    } else if (source == CA_STORE_ENV) {
        /* Environment overrides claim only the origin, so proxies still need the system store
         * discovered when libcurl's default is invalid. */
        ca_resolve("", NULL, NULL, NULL, info->ssl_version, features, info->cainfo, info->capath,
                   &proxy_file, &proxy_dir);
    }
#endif
}

void ca_apply(CURL *curl)
{
    if (store_file)
        curl_easy_setopt(curl, CURLOPT_CAINFO, store_file);
    if (store_dir) {
        /* Only an environment SSL_CERT_DIR can be unsupported here; probing already skips
         * backends without CAPATH support. */
        CURLcode result = curl_easy_setopt(curl, CURLOPT_CAPATH, store_dir);
        if (result == CURLE_NOT_BUILT_IN || result == CURLE_UNKNOWN_OPTION)
            warn_capath_ignored();
    }

    /* HTTPS proxies (https_proxy in the environment) verify with their own options, always
     * against the system store: environment overrides follow curl's origin-only semantics. */
    if (proxy_file)
        curl_easy_setopt(curl, CURLOPT_PROXY_CAINFO, proxy_file);
    if (proxy_dir)
        curl_easy_setopt(curl, CURLOPT_PROXY_CAPATH, proxy_dir);
}

const char *ca_verify_hint(CURLcode code)
{
    if (!store_missing)
        return NULL;
    if (code != CURLE_PEER_FAILED_VERIFICATION && code != CURLE_SSL_CACERT_BADFILE)
        return NULL;
    return "no CA certificate store found on this system; "
           "install the ca-certificates package or set SSL_CERT_FILE";
}

void ca_warn_verify_failure(CURLcode code)
{
    static atomic_int warned;
    const char *hint = ca_verify_hint(code);

    if (!hint || atomic_exchange(&warned, 1))
        return;
    hax_warn("%s", hint);
}
