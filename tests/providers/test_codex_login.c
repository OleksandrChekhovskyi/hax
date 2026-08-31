/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "xalloc.h"
#include "providers/codex_login.h"
#include "text/base64.h"

/* Build a JWT whose payload is `claims`; the header and signature are never inspected. */
static char *make_jwt(const char *claims)
{
    char *payload = base64url_encode(claims, strlen(claims), NULL);
    char *jwt = xasprintf("aGVhZGVy.%s.c2ln", payload);
    free(payload);
    return jwt;
}

static void test_parse_usercode(void)
{
    struct codex_device_auth device_auth;
    EXPECT(codex_login_parse_usercode(
               "{\"device_auth_id\":\"da-1\",\"user_code\":\"ABCD-EFGH\",\"interval\":\"7\"}",
               &device_auth) == 0);
    EXPECT_STR_EQ(device_auth.device_auth_id, "da-1");
    EXPECT_STR_EQ(device_auth.user_code, "ABCD-EFGH");
    EXPECT(device_auth.interval_s == 7);
    codex_device_auth_release(&device_auth);
}

/* The endpoint has reported the interval both as a string and as a number, and older responses
 * spell the code field "usercode". */
static void test_parse_usercode_variants(void)
{
    struct codex_device_auth device_auth;
    EXPECT(codex_login_parse_usercode(
               "{\"device_auth_id\":\"da-1\",\"usercode\":\"CODE\",\"interval\":3}",
               &device_auth) == 0);
    EXPECT_STR_EQ(device_auth.user_code, "CODE");
    EXPECT(device_auth.interval_s == 3);
    codex_device_auth_release(&device_auth);

    /* Missing, zero, and absurd intervals resolve to usable polling rates. */
    EXPECT(codex_login_parse_usercode("{\"device_auth_id\":\"da\",\"user_code\":\"C\"}",
                                      &device_auth) == 0);
    EXPECT(device_auth.interval_s == 5);
    codex_device_auth_release(&device_auth);

    EXPECT(
        codex_login_parse_usercode("{\"device_auth_id\":\"da\",\"user_code\":\"C\",\"interval\":0}",
                                   &device_auth) == 0);
    EXPECT(device_auth.interval_s == 5);
    codex_device_auth_release(&device_auth);

    EXPECT(codex_login_parse_usercode(
               "{\"device_auth_id\":\"da\",\"user_code\":\"C\",\"interval\":600}", &device_auth) ==
           0);
    EXPECT(device_auth.interval_s == 60);
    codex_device_auth_release(&device_auth);
}

static void test_parse_usercode_rejects_incomplete(void)
{
    struct codex_device_auth device_auth;
    EXPECT(codex_login_parse_usercode(NULL, &device_auth) == -1);
    EXPECT(codex_login_parse_usercode("not json", &device_auth) == -1);
    EXPECT(codex_login_parse_usercode("{}", &device_auth) == -1);
    EXPECT(codex_login_parse_usercode("{\"device_auth_id\":\"da\"}", &device_auth) == -1);
    EXPECT(codex_login_parse_usercode("{\"user_code\":\"C\"}", &device_auth) == -1);
    EXPECT(device_auth.device_auth_id == NULL);
    EXPECT(device_auth.user_code == NULL);
}

static void test_poll_authorized(void)
{
    char *code = NULL;
    char *verifier = NULL;
    EXPECT(codex_login_classify_poll(200,
                                     "{\"authorization_code\":\"ac\",\"code_verifier\":\"cv\"}",
                                     &code, &verifier) == CODEX_POLL_AUTHORIZED);
    EXPECT_STR_EQ(code, "ac");
    EXPECT_STR_EQ(verifier, "cv");
    free(code);
    free(verifier);
}

static void test_poll_pending_and_errors(void)
{
    char *code = NULL;
    char *verifier = NULL;
    /* The endpoint signals "not yet approved" as plain 403/404. */
    EXPECT(codex_login_classify_poll(403, NULL, &code, &verifier) == CODEX_POLL_PENDING);
    EXPECT(codex_login_classify_poll(404, "{}", &code, &verifier) == CODEX_POLL_PENDING);
    EXPECT(codex_login_classify_poll(400, "{\"error\":\"deviceauth_authorization_pending\"}", &code,
                                     &verifier) == CODEX_POLL_PENDING);
    EXPECT(codex_login_classify_poll(429, "{\"error\":{\"code\":\"slow_down\"}}", &code,
                                     &verifier) == CODEX_POLL_SLOW_DOWN);
    EXPECT(codex_login_classify_poll(400, "{\"error\":\"access_denied\"}", &code, &verifier) ==
           CODEX_POLL_FAILED);
    EXPECT(codex_login_classify_poll(500, NULL, &code, &verifier) == CODEX_POLL_FAILED);
    /* A 2xx without the code pair cannot proceed to the exchange. */
    EXPECT(codex_login_classify_poll(200, "{\"authorization_code\":\"ac\"}", &code, &verifier) ==
           CODEX_POLL_FAILED);
    EXPECT(code == NULL);
    EXPECT(verifier == NULL);
}

static void test_exchange_body_encoding(void)
{
    char *body = codex_login_build_exchange_body("a c/d", "verif~123",
                                                 "https://auth.openai.com/deviceauth/callback");
    EXPECT(body != NULL);
    if (body) {
        EXPECT(strstr(body, "grant_type=authorization_code&code=a%20c%2Fd&") != NULL);
        EXPECT(
            strstr(body, "&redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback&") !=
            NULL);
        EXPECT(strstr(body, "&code_verifier=verif~123") != NULL);
        EXPECT(strstr(body, "&client_id=") != NULL);
        free(body);
    }
}

static void test_authorize_url(void)
{
    char *url =
        codex_login_build_authorize_url("chall+65", "st&te", "http://localhost:1455/auth/callback");
    EXPECT(url != NULL);
    if (url) {
        EXPECT(strstr(url, "https://auth.openai.com/oauth/authorize?response_type=code"
                           "&client_id=") == url);
        EXPECT(strstr(url, "&redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback&") !=
               NULL);
        EXPECT(strstr(url, "&scope=openid%20profile%20email%20offline_access&") != NULL);
        EXPECT(strstr(url, "&code_challenge=chall%2B65&") != NULL);
        EXPECT(strstr(url, "&code_challenge_method=S256&") != NULL);
        EXPECT(strstr(url, "&state=st%26te&") != NULL);
        EXPECT(strstr(url, "&id_token_add_organizations=true") != NULL);
        EXPECT(strstr(url, "&codex_cli_simplified_flow=true") != NULL);
        EXPECT(strstr(url, "&originator=hax") != NULL);
        free(url);
    }
}

static char *exchange_response(const char *id_claims, const char *access_claims)
{
    char *id_token = make_jwt(id_claims);
    char *access_token = make_jwt(access_claims);
    char *body = xasprintf("{\"id_token\":\"%s\",\"access_token\":\"%s\",\"refresh_token\":\"rt\"}",
                           id_token, access_token);
    free(id_token);
    free(access_token);
    return body;
}

#define ACCOUNT_CLAIM "{\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"acc-1\"}}"

static void test_entry_from_exchange(void)
{
    char *body = exchange_response(ACCOUNT_CLAIM, "{}");
    json_t *entry = codex_login_entry_from_exchange(body);
    EXPECT(entry != NULL);
    if (entry) {
        EXPECT_STR_EQ(json_string_value(json_object_get(entry, "account_id")), "acc-1");
        EXPECT_STR_EQ(json_string_value(json_object_get(entry, "refresh_token")), "rt");
        EXPECT(json_object_get(entry, "access_token") != NULL);
        EXPECT(json_object_get(entry, "id_token") != NULL);
        json_decref(entry);
    }
    free(body);

    /* The account id claim may live on the access token instead. */
    body = exchange_response("{}", ACCOUNT_CLAIM);
    entry = codex_login_entry_from_exchange(body);
    EXPECT(entry != NULL);
    if (entry) {
        EXPECT_STR_EQ(json_string_value(json_object_get(entry, "account_id")), "acc-1");
        json_decref(entry);
    }
    free(body);
}

static void test_entry_from_exchange_rejects_incomplete(void)
{
    EXPECT(codex_login_entry_from_exchange(NULL) == NULL);
    EXPECT(codex_login_entry_from_exchange("not json") == NULL);
    EXPECT(codex_login_entry_from_exchange("{\"access_token\":\"at\"}") == NULL);

    /* Without an account id claim the credential cannot authenticate requests. */
    char *body = exchange_response("{}", "{}");
    EXPECT(codex_login_entry_from_exchange(body) == NULL);
    free(body);
}

static void test_apply_refresh_merges(void)
{
    json_t *entry = json_pack("{s:s, s:s, s:s, s:s}", "access_token", "old-at", "refresh_token",
                              "old-rt", "id_token", "old-id", "account_id", "acc-1");

    EXPECT(codex_login_apply_refresh(
               entry, "{\"access_token\":\"new-at\",\"refresh_token\":\"new-rt\"}") == 0);
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "access_token")), "new-at");
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "refresh_token")), "new-rt");
    /* Omitted fields keep their stored values; the account id never changes after login. */
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "id_token")), "old-id");
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "account_id")), "acc-1");

    /* A response without a new access token cannot have refreshed anything, and a failed merge
     * must not damage the stored rotation state. */
    EXPECT(codex_login_apply_refresh(entry, "{\"refresh_token\":\"other\"}") == -1);
    EXPECT(codex_login_apply_refresh(entry, "not json") == -1);
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "refresh_token")), "new-rt");
    json_decref(entry);
}

static void test_token_as_fresh(void)
{
    char *old_token = make_jwt("{\"exp\":1000}");
    char *new_token = make_jwt("{\"exp\":2000}");
    char *no_exp = make_jwt("{}");

    EXPECT(codex_login_token_as_fresh(new_token, old_token));
    EXPECT(codex_login_token_as_fresh(new_token, new_token));
    EXPECT(!codex_login_token_as_fresh(old_token, new_token));
    /* Opaque expiries cannot be ordered, so adoption stays possible. */
    EXPECT(codex_login_token_as_fresh(no_exp, new_token));
    EXPECT(codex_login_token_as_fresh(old_token, no_exp));
    EXPECT(codex_login_token_as_fresh(NULL, old_token));

    free(old_token);
    free(new_token);
    free(no_exp);
}

/* Deleting a stored login requires an explicit terminal OAuth rejection; misleading statuses
 * from proxies and gateways must stay retryable. */
static void test_refresh_rejected(void)
{
    EXPECT(codex_login_refresh_rejected(400, "{\"error\":\"invalid_grant\"}"));
    EXPECT(codex_login_refresh_rejected(401, "{\"error\":{\"code\":\"refresh_token_reused\"}}"));
    EXPECT(codex_login_refresh_rejected(403, "{\"code\":\"refresh_token_invalidated\"}"));

    EXPECT(!codex_login_refresh_rejected(403, NULL)); /* WAF block page */
    EXPECT(!codex_login_refresh_rejected(403, "<html>denied</html>"));
    EXPECT(!codex_login_refresh_rejected(400, "{\"error\":\"rate_limited\"}"));
    EXPECT(!codex_login_refresh_rejected(302, NULL)); /* captive portal */
    EXPECT(!codex_login_refresh_rejected(500, "{\"error\":\"invalid_grant\"}")); /* 4xx only */
    EXPECT(!codex_login_refresh_rejected(200, NULL));
}

int main(void)
{
    test_parse_usercode();
    test_parse_usercode_variants();
    test_parse_usercode_rejects_incomplete();
    test_poll_authorized();
    test_poll_pending_and_errors();
    test_exchange_body_encoding();
    test_authorize_url();
    test_entry_from_exchange();
    test_entry_from_exchange_rejects_incomplete();
    test_apply_refresh_merges();
    test_token_as_fresh();
    test_refresh_rejected();
    T_REPORT();
}
