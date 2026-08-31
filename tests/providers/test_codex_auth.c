/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cred_store.h"
#include "harness.h"
#include "xalloc.h"
#include "providers/codex_auth.h"
#include "providers/http_provider.h"
#include "text/base64.h"

/* Build a JWT whose payload is `claims`; the header and signature are never inspected. */
static char *make_jwt(const char *claims)
{
    char *payload = base64url_encode(claims, strlen(claims), NULL);
    char *jwt = xasprintf("aGVhZGVy.%s.c2ln", payload);
    free(payload);
    return jwt;
}

static void expect_email(const char *claims, const char *want)
{
    char *jwt = make_jwt(claims);
    char *email = codex_jwt_email(jwt);
    EXPECT_STR_EQ(email, want);
    free(email);
    free(jwt);
}

static void expect_no_email(const char *claims)
{
    char *jwt = make_jwt(claims);
    char *email = codex_jwt_email(jwt);
    EXPECT(email == NULL);
    free(email);
    free(jwt);
}

static void test_email_claim(void)
{
    expect_email("{\"email\":\"user@example.com\"}", "user@example.com");
}

/* Some login flows carry the email only under the namespaced profile claim. */
static void test_profile_claim_fallback(void)
{
    expect_email("{\"https://api.openai.com/profile\":{\"email\":\"p@example.com\"}}",
                 "p@example.com");
    expect_email(
        "{\"email\":\"\",\"https://api.openai.com/profile\":{\"email\":\"p@example.com\"}}",
        "p@example.com");
}

static void test_top_level_claim_wins(void)
{
    expect_email("{\"email\":\"top@example.com\","
                 "\"https://api.openai.com/profile\":{\"email\":\"p@example.com\"}}",
                 "top@example.com");
}

static void test_missing_email(void)
{
    expect_no_email("{}");
    expect_no_email("{\"email\":\"\"}");
    expect_no_email("{\"sub\":\"abc\"}");
    expect_no_email("{\"https://api.openai.com/profile\":{}}");
    expect_no_email("{\"email\":null}");
    expect_no_email("{\"email\":42}");
}

/* The payload comes from a file another program wrote, so malformed shapes must be survivable. */
static void test_malformed_token(void)
{
    EXPECT(codex_jwt_email(NULL) == NULL);
    EXPECT(codex_jwt_email("") == NULL);
    EXPECT(codex_jwt_email("no-dots-at-all") == NULL);
    EXPECT(codex_jwt_email("header.only-one-dot") == NULL);
    EXPECT(codex_jwt_email("header..signature") == NULL);
    EXPECT(codex_jwt_email("header.!!!not-base64!!!.signature") == NULL);
    EXPECT(codex_jwt_email("...") == NULL);
}

static void test_payload_not_json(void)
{
    char *jwt = make_jwt("not json at all");
    EXPECT(codex_jwt_email(jwt) == NULL);
    free(jwt);

    jwt = make_jwt("[1,2,3]");
    EXPECT(codex_jwt_email(jwt) == NULL);
    free(jwt);
}

static enum codex_auth_status status_of(const char *json, struct codex_auth *auth)
{
    json_t *root = json_loads(json, 0, NULL);
    EXPECT(root != NULL);
    enum codex_auth_status status = codex_auth_from_json(root, auth);
    json_decref(root);
    return status;
}

static void test_tokens_read(void)
{
    struct codex_auth auth;
    EXPECT(status_of("{\"tokens\":{\"access_token\":\"at\",\"account_id\":\"acc\"}}", &auth) ==
           CODEX_AUTH_OK);
    EXPECT_STR_EQ(auth.access_token, "at");
    EXPECT_STR_EQ(auth.account_id, "acc");
    EXPECT(auth.email == NULL);
    codex_auth_release(&auth);
}

static void test_tokens_carry_email(void)
{
    char *jwt = make_jwt("{\"email\":\"user@example.com\"}");
    char *json = xasprintf("{\"tokens\":{\"access_token\":\"at\",\"account_id\":\"acc\","
                           "\"id_token\":\"%s\"}}",
                           jwt);
    struct codex_auth auth;
    EXPECT(status_of(json, &auth) == CODEX_AUTH_OK);
    EXPECT_STR_EQ(auth.email, "user@example.com");
    codex_auth_release(&auth);
    free(json);
    free(jwt);
}

static void test_incomplete_tokens_rejected(void)
{
    struct codex_auth auth;
    EXPECT(status_of("{}", &auth) == CODEX_AUTH_NO_TOKENS);
    EXPECT(status_of("{\"tokens\":{}}", &auth) == CODEX_AUTH_NO_TOKENS);
    EXPECT(status_of("{\"tokens\":{\"access_token\":\"at\"}}", &auth) == CODEX_AUTH_NO_TOKENS);
    EXPECT(status_of("{\"tokens\":{\"account_id\":\"acc\"}}", &auth) == CODEX_AUTH_NO_TOKENS);
    EXPECT(status_of("{\"tokens\":{\"access_token\":\"\",\"account_id\":\"acc\"}}", &auth) ==
           CODEX_AUTH_NO_TOKENS);
    EXPECT(status_of("{\"tokens\":{\"access_token\":\"at\",\"account_id\":\"\"}}", &auth) ==
           CODEX_AUTH_NO_TOKENS);
    EXPECT(status_of("{\"tokens\":\"not-an-object\"}", &auth) == CODEX_AUTH_NO_TOKENS);
    EXPECT(status_of("{\"tokens\":{\"access_token\":7,\"account_id\":\"acc\"}}", &auth) ==
           CODEX_AUTH_NO_TOKENS);
}

/* A rejected load must leave nothing to free and nothing stale to read. */
static void test_rejected_load_zeroes_output(void)
{
    struct codex_auth auth;
    EXPECT(status_of("{\"tokens\":{}}", &auth) == CODEX_AUTH_NO_TOKENS);
    EXPECT(auth.access_token == NULL);
    EXPECT(auth.account_id == NULL);
    EXPECT(auth.email == NULL);
    codex_auth_release(&auth);
}

/* Point HOME at a scratch directory so the loader reads a file the test controls. The hax
 * credential store lookup must also resolve under the scratch home, not the developer's. */
static char *auth_home(void)
{
    char *home = t_tempdir();
    char *codex_dir = xasprintf("%s/.codex", home);
    EXPECT(mkdir(codex_dir, 0700) == 0);
    free(codex_dir);
    setenv("HOME", home, 1);
    unsetenv("XDG_STATE_HOME");
    return home;
}

static void write_auth(const char *home, const char *contents)
{
    char *path = xasprintf("%s/.codex/auth.json", home);
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (file) {
        fputs(contents, file);
        fclose(file);
    }
    free(path);
}

static void test_load_missing_file(void)
{
    auth_home();
    struct codex_auth auth;
    char *detail = NULL;
    EXPECT(codex_auth_load(&auth, &detail) == CODEX_AUTH_NO_FILE);
    EXPECT(auth.access_token == NULL);
    EXPECT(detail != NULL);
    if (detail)
        EXPECT(strstr(detail, "/.codex/auth.json") != NULL);
    free(detail);
}

static void test_load_bad_json(void)
{
    char *home = auth_home();
    write_auth(home, "{not json");

    struct codex_auth auth;
    char *detail = NULL;
    EXPECT(codex_auth_load(&auth, &detail) == CODEX_AUTH_BAD_JSON);
    EXPECT(detail != NULL);
    free(detail);
}

static void test_load_valid_file(void)
{
    char *home = auth_home();
    write_auth(home, "{\"tokens\":{\"access_token\":\"at\",\"account_id\":\"acc\"}}");

    struct codex_auth auth;
    char *detail = NULL;
    EXPECT(codex_auth_load(&auth, &detail) == CODEX_AUTH_OK);
    EXPECT(detail == NULL);
    EXPECT_STR_EQ(auth.access_token, "at");
    EXPECT_STR_EQ(auth.account_id, "acc");
    codex_auth_release(&auth);
}

/* Reloading after the codex CLI rewrites auth.json is what lets a refreshed token be picked up
 * without restarting, so a second load must see the new credentials. */
static void test_load_sees_rewritten_credentials(void)
{
    char *home = auth_home();
    write_auth(home, "{\"tokens\":{\"access_token\":\"old\",\"account_id\":\"acc1\"}}");

    struct codex_auth auth;
    EXPECT(codex_auth_load(&auth, NULL) == CODEX_AUTH_OK);
    EXPECT_STR_EQ(auth.access_token, "old");
    codex_auth_release(&auth);

    write_auth(home, "{\"tokens\":{\"access_token\":\"new\",\"account_id\":\"acc2\"}}");
    EXPECT(codex_auth_load(&auth, NULL) == CODEX_AUTH_OK);
    EXPECT_STR_EQ(auth.access_token, "new");
    EXPECT_STR_EQ(auth.account_id, "acc2");
    codex_auth_release(&auth);
}

/* A failed reload must leave the caller's existing credentials usable rather than half-replaced. */
static void test_failed_reload_reports_zeroed_auth(void)
{
    char *home = auth_home();
    write_auth(home, "{\"tokens\":{}}");

    struct codex_auth auth;
    EXPECT(codex_auth_load(&auth, NULL) == CODEX_AUTH_NO_TOKENS);
    EXPECT(auth.access_token == NULL);
    EXPECT(auth.account_id == NULL);
    EXPECT(auth.email == NULL);
    codex_auth_release(&auth);
}

static void test_equal_compares_both_header_values(void)
{
    struct codex_auth base = {.access_token = (char *)"at", .account_id = (char *)"acc"};
    struct codex_auth same = {.access_token = (char *)"at", .account_id = (char *)"acc"};
    struct codex_auth other_token = {.access_token = (char *)"at2", .account_id = (char *)"acc"};
    struct codex_auth other_account = {.access_token = (char *)"at", .account_id = (char *)"acc2"};

    EXPECT(codex_auth_equal(&base, &same));
    EXPECT(!codex_auth_equal(&base, &other_token));
    EXPECT(!codex_auth_equal(&base, &other_account));
}

/* The email is a display label rather than something sent, so it must not force a reload. */
static void test_equal_ignores_email(void)
{
    struct codex_auth with_email = {.access_token = (char *)"at",
                                    .account_id = (char *)"acc",
                                    .email = (char *)"a@example.com"};
    struct codex_auth other_email = {.access_token = (char *)"at",
                                     .account_id = (char *)"acc",
                                     .email = (char *)"b@example.com"};
    struct codex_auth no_email = {.access_token = (char *)"at", .account_id = (char *)"acc"};

    EXPECT(codex_auth_equal(&with_email, &other_email));
    EXPECT(codex_auth_equal(&with_email, &no_email));
}

/* Released credentials are zeroed, and comparing them must not dereference NULL. */
static void test_equal_handles_zeroed(void)
{
    struct codex_auth zeroed = {0};
    struct codex_auth loaded = {.access_token = (char *)"at", .account_id = (char *)"acc"};
    struct codex_auth partial = {.access_token = (char *)"at"};

    EXPECT(codex_auth_equal(&zeroed, &zeroed));
    EXPECT(!codex_auth_equal(&zeroed, &loaded));
    EXPECT(!codex_auth_equal(&loaded, &zeroed));
    EXPECT(!codex_auth_equal(&partial, &loaded));
}

static void test_jwt_exp(void)
{
    char *jwt = make_jwt("{\"exp\":1755500000}");
    EXPECT(codex_jwt_exp(jwt) == 1755500000);
    free(jwt);

    jwt = make_jwt("{}");
    EXPECT(codex_jwt_exp(jwt) == 0);
    free(jwt);

    jwt = make_jwt("{\"exp\":\"soon\"}");
    EXPECT(codex_jwt_exp(jwt) == 0);
    free(jwt);

    EXPECT(codex_jwt_exp(NULL) == 0);
    EXPECT(codex_jwt_exp("not-a-jwt") == 0);
}

static void test_jwt_account_id(void)
{
    char *jwt = make_jwt("{\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"acc-42\"}}");
    char *account_id = codex_jwt_account_id(jwt);
    EXPECT_STR_EQ(account_id, "acc-42");
    free(account_id);
    free(jwt);

    jwt = make_jwt("{\"https://api.openai.com/auth\":{}}");
    EXPECT(codex_jwt_account_id(jwt) == NULL);
    free(jwt);

    jwt = make_jwt("{}");
    EXPECT(codex_jwt_account_id(jwt) == NULL);
    free(jwt);
}

static void test_store_entry_read(void)
{
    json_t *entry = json_pack("{s:s, s:s, s:s}", "access_token", "at", "refresh_token", "rt",
                              "account_id", "acc");
    struct codex_auth auth;
    EXPECT(codex_auth_from_store_entry(entry, &auth) == CODEX_AUTH_OK);
    EXPECT_STR_EQ(auth.access_token, "at");
    EXPECT_STR_EQ(auth.refresh_token, "rt");
    EXPECT_STR_EQ(auth.account_id, "acc");
    EXPECT(auth.source == CODEX_AUTH_SOURCE_HAX);
    codex_auth_release(&auth);

    /* Without a refresh token the entry cannot sustain hax-managed rotation. */
    json_object_del(entry, "refresh_token");
    EXPECT(codex_auth_from_store_entry(entry, &auth) == CODEX_AUTH_NO_TOKENS);
    EXPECT(auth.access_token == NULL);
    json_decref(entry);
}

/* A hax-owned login outranks borrowed codex CLI credentials, and removing it falls back. */
static void test_load_prefers_hax_store(void)
{
    char *home = auth_home();
    write_auth(home, "{\"tokens\":{\"access_token\":\"cli-at\",\"account_id\":\"cli-acc\"}}");

    json_t *entry = json_pack("{s:s, s:s, s:s}", "access_token", "hax-at", "refresh_token",
                              "hax-rt", "account_id", "hax-acc");
    EXPECT(cred_store_set("codex", entry) == 0);
    json_decref(entry);

    struct codex_auth auth;
    EXPECT(codex_auth_load(&auth, NULL) == CODEX_AUTH_OK);
    EXPECT(auth.source == CODEX_AUTH_SOURCE_HAX);
    EXPECT_STR_EQ(auth.access_token, "hax-at");
    EXPECT_STR_EQ(auth.refresh_token, "hax-rt");
    codex_auth_release(&auth);

    EXPECT(cred_store_delete("codex") == 1);
    EXPECT(codex_auth_load(&auth, NULL) == CODEX_AUTH_OK);
    EXPECT(auth.source == CODEX_AUTH_SOURCE_CODEX_CLI);
    EXPECT_STR_EQ(auth.access_token, "cli-at");
    EXPECT(auth.refresh_token == NULL);
    codex_auth_release(&auth);
}

static int headers_contain(char **headers, const char *needle)
{
    for (char **header = headers; header && *header; header++)
        if (strstr(*header, needle))
            return 1;
    return 0;
}

/* The credential session behind the auth-source hook, over borrowed CLI credentials: prepare
 * tracks the live login, headers carry the credentials plus the session routing, recovery
 * declines (only the CLI can refresh its token), and a 401 marks the credentials stale so the
 * next prepare adopts a token the CLI rewrote meanwhile. */
static void test_auth_session(void)
{
    char *home = auth_home();
    write_auth(home, "{\"tokens\":{\"access_token\":\"at\",\"account_id\":\"acc\"}}");

    struct http_auth_source source = {0};
    EXPECT(codex_auth_source(NULL, &source) == 0);
    EXPECT(source.ops != NULL);
    if (!source.ops)
        return;

    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);
    char **headers = source.ops->headers(source.state, "sess", 1);
    EXPECT(headers_contain(headers, "Authorization: Bearer at"));
    EXPECT(headers_contain(headers, "chatgpt-account-id: acc"));
    EXPECT(headers_contain(headers, "session-id: sess"));
    EXPECT(headers_contain(headers, "x-client-request-id: sess"));
    string_array_free(headers);
    /* Borrowed tokens never count as expiring: the probe path must not defer on them. */
    EXPECT(!codex_auth_session_expiring(source.state, 1000000));

    int retried = 0;
    EXPECT(source.ops->recover(source.state, &retried, NULL, NULL) == 0);
    char *message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "codex CLI token expired") != NULL);
    free(message);

    /* The 401 above marked the borrowed credentials stale; prepare re-reads auth.json. */
    write_auth(home, "{\"tokens\":{\"access_token\":\"renewed\",\"account_id\":\"acc\"}}");
    EXPECT(source.ops->prepare(source.state, 0, NULL, NULL) == 0);
    headers = source.ops->headers(source.state, "sess", 0);
    EXPECT(headers_contain(headers, "Authorization: Bearer renewed"));
    string_array_free(headers);

    /* /logout with nothing left behind: the session clears and requests report the state. */
    char *path = xasprintf("%s/.codex/auth.json", home);
    EXPECT(remove(path) == 0);
    free(path);
    codex_auth_session_reload(source.state);
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) != 0);
    message = source.ops->unauthorized_message(source.state);
    EXPECT(strstr(message, "not logged in") != NULL);
    free(message);

    /* A later login is adopted without rebuilding the provider. */
    write_auth(home, "{\"tokens\":{\"access_token\":\"back\",\"account_id\":\"acc\"}}");
    EXPECT(source.ops->prepare(source.state, 1, NULL, NULL) == 0);

    source.ops->destroy(source.state);
}

static void test_status_reasons(void)
{
    EXPECT(codex_auth_status_reason(CODEX_AUTH_OK) == NULL);
    EXPECT_STR_EQ(codex_auth_status_reason(CODEX_AUTH_NO_FILE), "not logged in (use /login)");
    EXPECT_STR_EQ(codex_auth_status_reason(CODEX_AUTH_NO_TOKENS), "not logged in (use /login)");
    EXPECT_STR_EQ(codex_auth_status_reason(CODEX_AUTH_BAD_JSON), "auth.json not valid JSON");
}

int main(void)
{
    test_email_claim();
    test_profile_claim_fallback();
    test_top_level_claim_wins();
    test_missing_email();
    test_malformed_token();
    test_payload_not_json();
    test_tokens_read();
    test_tokens_carry_email();
    test_incomplete_tokens_rejected();
    test_rejected_load_zeroes_output();
    test_load_missing_file();
    test_load_bad_json();
    test_load_valid_file();
    test_load_sees_rewritten_credentials();
    test_failed_reload_reports_zeroed_auth();
    test_equal_compares_both_header_values();
    test_equal_ignores_email();
    test_equal_handles_zeroed();
    test_jwt_exp();
    test_jwt_account_id();
    test_store_entry_read();
    test_load_prefers_hax_store();
    test_auth_session();
    test_status_reasons();
    T_REPORT();
}
