/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cred_store.h"
#include "harness.h"
#include "providers/anthropic_login.h"
#include "transport/oauth.h"

static void test_authorization_code_and_state(void)
{
    char *url = anthropic_login_authorize_url("challenge", "state+value");
    EXPECT(strstr(url, "https://claude.ai/oauth/authorize?code=true&") == url);
    EXPECT(strstr(url, "code_challenge=challenge&code_challenge_method=S256") != NULL);
    char *state = oauth_query_param(strchr(url, '?') + 1, "state");
    EXPECT_STR_EQ(state, "state+value");
    free(state);
    free(url);
    char *body = anthropic_login_code_body("secret-code#state", "verifier", "state");
    EXPECT(body != NULL);
    json_t *root = body ? json_loads(body, 0, NULL) : NULL;
    EXPECT_STR_EQ(json_string_value(json_object_get(root, "code")), "secret-code");
    EXPECT_STR_EQ(json_string_value(json_object_get(root, "state")), "state");
    EXPECT_STR_EQ(json_string_value(json_object_get(root, "code_verifier")), "verifier");
    EXPECT_STR_EQ(json_string_value(json_object_get(root, "redirect_uri")),
                  "https://console.anthropic.com/oauth/code/callback");
    json_decref(root);
    free(body);
    const char *invalid[] = {NULL,          "", "code", "#state", "code#wrong", "code#state#extra",
                             "code\n#state"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        EXPECT(anthropic_login_code_body(invalid[i], "verifier", "state") == NULL);
}

static void test_tokens_and_refresh(void)
{
    json_t *entry = anthropic_login_parse_token(
        "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600}", NULL);
    EXPECT(entry != NULL);
    json_object_set_new(entry, "login_id", json_string("login"));
    EXPECT(anthropic_login_entry_valid(entry));
    EXPECT(json_integer_value(json_object_get(entry, "expires_at")) > (json_int_t)time(NULL));
    json_t *rotated = anthropic_login_parse_token(
        "{\"access_token\":\"new-access\",\"expires_in\":1800,\"token_type\":\"Bearer\"}", entry);
    EXPECT(anthropic_login_entry_valid(rotated));
    EXPECT_STR_EQ(json_string_value(json_object_get(rotated, "refresh_token")), "refresh");
    EXPECT_STR_EQ(json_string_value(json_object_get(rotated, "login_id")), "login");
    EXPECT(json_integer_value(json_object_get(rotated, "generation")) == 1);
    json_decref(rotated);
    char *body = anthropic_login_refresh_body("refresh\"+secret");
    json_t *root = json_loads(body, 0, NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(root, "refresh_token")), "refresh\"+secret");
    EXPECT_STR_EQ(json_string_value(json_object_get(root, "grant_type")), "refresh_token");
    json_decref(root);
    free(body);
    const char *invalid[] = {
        NULL,
        "{}",
        "[]",
        "not-json",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":0}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":\"3600\"}",
        "{\"access_token\":\"a\\r\\nX-Evil: yes\",\"refresh_token\":\"r\",\"expires_in\":3600}",
        "{\"access_token\":\"a\",\"refresh_token\":\"r\",\"expires_in\":3600,\"token_type\":"
        "\"Basic\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        EXPECT(anthropic_login_parse_token(invalid[i], NULL) == NULL);
    json_object_set_new(entry, "generation", json_integer(INT64_MAX - 1));
    EXPECT(anthropic_login_parse_token("{\"access_token\":\"a\",\"expires_in\":3600}", entry) ==
           NULL);
    json_decref(entry);
    EXPECT(anthropic_login_refresh_rejected(400, "{\"error\":\"invalid_grant\"}"));
    EXPECT(!anthropic_login_refresh_rejected(503, "{\"error\":\"invalid_grant\"}"));
    EXPECT(!anthropic_login_refresh_rejected(401, "<html>invalid_grant</html>"));
    EXPECT(!anthropic_login_refresh_rejected(429, "{\"error\":\"rate_limit\"}"));
}

static void test_local_login_status_and_logout(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    EXPECT(!anthropic_login_present());
    EXPECT(anthropic_login_status() == NULL);
    EXPECT(anthropic_logout_run() == 0);
    json_t *entry = anthropic_login_parse_token(
        "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600}", NULL);
    json_object_set_new(entry, "login_id", json_string("login"));
    EXPECT(cred_store_set("anthropic", entry) == 0);
    json_decref(entry);
    EXPECT(anthropic_login_present());
    char *status = anthropic_login_status();
    EXPECT(status != NULL);
    free(status);
    EXPECT(anthropic_logout_run() == 1);
    EXPECT(!anthropic_login_present());
}

int main(void)
{
    test_authorization_code_and_state();
    test_tokens_and_refresh();
    test_local_login_status_and_logout();
    T_REPORT();
}
