/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "harness.h"
#include "transport/ca.h"

/* Creates every directory component of root + rel; rel must start with '/'. */
static void make_dir(const char *root, const char *rel)
{
    char path[1024];
    int len = snprintf(path, sizeof(path), "%s%s", root, rel);
    EXPECT(len > 0 && (size_t)len < sizeof(path));

    for (char *slash = path + strlen(root) + 1; (slash = strchr(slash, '/')); slash++) {
        *slash = '\0';
        mkdir(path, 0755);
        *slash = '/';
    }
    mkdir(path, 0755);
}

static void make_file(const char *root, const char *rel)
{
    char rel_dir[1024];
    int len = snprintf(rel_dir, sizeof(rel_dir), "%s", rel);
    EXPECT(len > 0 && (size_t)len < sizeof(rel_dir));
    *strrchr(rel_dir, '/') = '\0';
    make_dir(root, rel_dir);

    char path[1024];
    len = snprintf(path, sizeof(path), "%s%s", root, rel);
    EXPECT(len > 0 && (size_t)len < sizeof(path));
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (file)
        fclose(file);
}

static void test_env_bundle_wins(void)
{
    char *file = NULL;
    char *dir = NULL;

    EXPECT(ca_resolve(t_tempdir(), "/env/bundle.pem", "/env/file.pem", "/env/dir", NULL, NULL,
                      "/curl/default.pem", NULL, &file, &dir) == CA_STORE_ENV);
    EXPECT_STR_EQ(file, "/env/bundle.pem");
    EXPECT(dir == NULL);
    free(file);
}

static void test_env_file_and_dir_combine(void)
{
    char *file = NULL;
    char *dir = NULL;

    EXPECT(ca_resolve(t_tempdir(), NULL, "/env/file.pem", "/env/dir", NULL, NULL, NULL, NULL, &file,
                      &dir) == CA_STORE_ENV);
    EXPECT_STR_EQ(file, "/env/file.pem");
    EXPECT_STR_EQ(dir, "/env/dir");
    free(file);
    free(dir);
}

static void test_env_file_used_without_bundle(void)
{
    char *file = NULL;
    char *dir = NULL;

    EXPECT(ca_resolve(t_tempdir(), NULL, "/env/file.pem", NULL, NULL, NULL, NULL, NULL, &file,
                      &dir) == CA_STORE_ENV);
    EXPECT_STR_EQ(file, "/env/file.pem");
    EXPECT(dir == NULL);
    free(file);
}

static void test_env_dir_alone_suppresses_probing(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, "/env/dir", NULL, NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_ENV);
    EXPECT(file == NULL);
    EXPECT_STR_EQ(dir, "/env/dir");
    free(dir);
}

static void test_empty_env_values_ignored(void)
{
    char *file = NULL;
    char *dir = NULL;

    EXPECT(ca_resolve(t_tempdir(), "", "", "", "OpenSSL/3.5.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_MISSING);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_valid_curl_default_left_alone(void)
{
    char *root = t_tempdir();
    make_file(root, "/curl/default.pem");
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, "/curl/default.pem", NULL,
                      &file, &dir) == CA_STORE_DEFAULT);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_valid_curl_default_dir_left_alone(void)
{
    char *root = t_tempdir();
    make_file(root, "/curl/certs/4042bcee.0");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, NULL, "/curl/certs", &file,
                      &dir) == CA_STORE_DEFAULT);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_unhashed_curl_default_dir_rejected(void)
{
    char *root = t_tempdir();
    make_dir(root, "/curl/certs");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, NULL, "/curl/certs", &file,
                      &dir) == CA_STORE_MISSING);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_missing_default_probes_known_paths(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");
    make_file(root, "/etc/ssl/certs/4042bcee.0");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, "/curl/default.pem", NULL,
                      &file, &dir) == CA_STORE_PROBED);

    char want_file[1024];
    char want_dir[1024];
    snprintf(want_file, sizeof(want_file), "%s/etc/ssl/certs/ca-certificates.crt", root);
    snprintf(want_dir, sizeof(want_dir), "%s/etc/ssl/certs", root);
    EXPECT_STR_EQ(file, want_file);
    EXPECT_STR_EQ(dir, want_dir);
    free(file);
    free(dir);
}

static void test_probe_prefers_debian_bundle(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");
    make_file(root, "/etc/pki/tls/certs/ca-bundle.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_PROBED);
    EXPECT(file != NULL && strstr(file, "/etc/ssl/certs/ca-certificates.crt") != NULL);
    free(file);
    free(dir);
}

static void test_probe_finds_fedora_bundle(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/pki/tls/certs/ca-bundle.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_PROBED);
    EXPECT(file != NULL && strstr(file, "/etc/pki/tls/certs/ca-bundle.crt") != NULL);
    EXPECT(dir == NULL);
    free(file);
}

static void test_probed_dir_needs_hashed_certs(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/pki/tls/certs/12ab34cd.0");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_PROBED);
    EXPECT(file == NULL);
    EXPECT(dir != NULL && strstr(dir, "/etc/pki/tls/certs") != NULL);
    free(dir);
}

static void test_empty_probed_dir_is_not_a_store(void)
{
    char *root = t_tempdir();
    make_dir(root, "/etc/ssl/certs");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_MISSING);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_gnutls_capath_accepts_plain_files(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/pki/tls/certs/site-ca.pem");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "GnuTLS/3.8.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_PROBED);
    EXPECT(file == NULL);
    EXPECT(dir != NULL && strstr(dir, "/etc/pki/tls/certs") != NULL);
    free(dir);
}

static void test_gnutls_curl_default_dir_with_plain_files_kept(void)
{
    char *root = t_tempdir();
    make_file(root, "/curl/certs/site-ca.pem");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "GnuTLS/3.8.0", NULL, NULL, "/curl/certs", &file,
                      &dir) == CA_STORE_DEFAULT);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_rustls_ignores_directories(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/4042bcee.0");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "rustls/0.15.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_MISSING);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_rustls_still_finds_bundle_files(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");
    make_file(root, "/etc/ssl/certs/4042bcee.0");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "rustls/0.15.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_PROBED);
    EXPECT(file != NULL && strstr(file, "/etc/ssl/certs/ca-certificates.crt") != NULL);
    EXPECT(dir == NULL);
    free(file);
}

static void test_gnutls_empty_dir_still_rejected(void)
{
    char *root = t_tempdir();
    make_dir(root, "/etc/ssl/certs");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "GnuTLS/3.8.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_MISSING);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_probe_finds_freebsd_bundle(void)
{
    char *root = t_tempdir();
    make_file(root, "/usr/local/share/certs/ca-root-nss.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_PROBED);
    EXPECT(file != NULL && strstr(file, "/usr/local/share/certs/ca-root-nss.crt") != NULL);
    EXPECT(dir == NULL);
    free(file);
}

static void test_native_backend_suppresses_probing(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "SecureTransport", NULL, NULL, NULL, &file, &dir) ==
           CA_STORE_DEFAULT);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_active_native_backend_in_multissl_suppresses_probing(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "(OpenSSL/3.5.0) SecureTransport", NULL, NULL, NULL,
                      &file, &dir) == CA_STORE_DEFAULT);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_inactive_native_backend_ignored(void)
{
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0 (SecureTransport)", NULL, NULL, NULL,
                      &file, &dir) == CA_STORE_PROBED);
    EXPECT(file != NULL && strstr(file, "/etc/ssl/certs/ca-certificates.crt") != NULL);
    free(file);
    free(dir);
}

static void test_native_trust_feature_suppresses_probing(void)
{
    static const char *const features[] = {"HTTP2", "AppleSecTrust", "SSL", NULL};
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "OpenSSL/3.5.0", features, NULL, NULL, &file, &dir) ==
           CA_STORE_DEFAULT);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_native_ca_feature_suppresses_probing(void)
{
    static const char *const features[] = {"NativeCA", NULL};
    char *root = t_tempdir();
    make_file(root, "/etc/ssl/certs/ca-certificates.crt");

    char *file = NULL;
    char *dir = NULL;
    EXPECT(ca_resolve(root, NULL, NULL, NULL, "GnuTLS/3.8.0", features, NULL, NULL, &file, &dir) ==
           CA_STORE_DEFAULT);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

static void test_env_wins_over_native_trust(void)
{
    static const char *const features[] = {"AppleSecTrust", NULL};
    char *file = NULL;
    char *dir = NULL;

    EXPECT(ca_resolve(t_tempdir(), NULL, "/env/file.pem", NULL, "Schannel", features, NULL, NULL,
                      &file, &dir) == CA_STORE_ENV);
    EXPECT_STR_EQ(file, "/env/file.pem");
    EXPECT(dir == NULL);
    free(file);
}

static void test_nothing_found(void)
{
    char *file = NULL;
    char *dir = NULL;

    EXPECT(ca_resolve(t_tempdir(), NULL, NULL, NULL, "OpenSSL/3.5.0", NULL, "/curl/default.pem",
                      "/curl/certs", &file, &dir) == CA_STORE_MISSING);
    EXPECT(file == NULL);
    EXPECT(dir == NULL);
}

int main(void)
{
    test_env_bundle_wins();
    test_env_file_and_dir_combine();
    test_env_file_used_without_bundle();
    test_env_dir_alone_suppresses_probing();
    test_empty_env_values_ignored();
    test_valid_curl_default_left_alone();
    test_valid_curl_default_dir_left_alone();
    test_unhashed_curl_default_dir_rejected();
    test_missing_default_probes_known_paths();
    test_probe_prefers_debian_bundle();
    test_probe_finds_fedora_bundle();
    test_probed_dir_needs_hashed_certs();
    test_empty_probed_dir_is_not_a_store();
    test_gnutls_capath_accepts_plain_files();
    test_gnutls_curl_default_dir_with_plain_files_kept();
    test_rustls_ignores_directories();
    test_rustls_still_finds_bundle_files();
    test_gnutls_empty_dir_still_rejected();
    test_probe_finds_freebsd_bundle();
    test_native_backend_suppresses_probing();
    test_active_native_backend_in_multissl_suppresses_probing();
    test_inactive_native_backend_ignored();
    test_native_trust_feature_suppresses_probing();
    test_native_ca_feature_suppresses_probing();
    test_env_wins_over_native_trust();
    test_nothing_found();
    T_REPORT();
}
