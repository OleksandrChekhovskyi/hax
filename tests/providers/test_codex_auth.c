/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"
#include "providers/codex_auth.h"
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

static void test_status_reasons(void)
{
    EXPECT(codex_auth_status_reason(CODEX_AUTH_OK) == NULL);
    EXPECT_STR_EQ(codex_auth_status_reason(CODEX_AUTH_NO_FILE), "codex CLI not logged in");
    EXPECT_STR_EQ(codex_auth_status_reason(CODEX_AUTH_NO_TOKENS), "codex CLI not logged in");
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
    test_status_reasons();
    T_REPORT();
}
