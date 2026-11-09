/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "config.h"
#include "cred_store.h"
#include "harness.h"
#include "trace.h"
#include "xalloc.h"
#include "providers/kimi_login.h"
#include "system/fs.h"

#define TOKEN_RESPONSE                                                                             \
    "{\"access_token\":\"access-secret\",\"refresh_token\":\"refresh-secret\","                    \
    "\"token_type\":\"Bearer\",\"expires_in\":3600}"

static void test_device_response(void)
{
    struct kimi_device_auth auth;
    EXPECT(kimi_login_parse_device(
               "{\"device_code\":\"private-device-code\",\"user_code\":\"ABCD-EFGH\","
               "\"verification_uri\":\"https://auth.kimi.com/device\","
               "\"verification_uri_complete\":\"https://auth.kimi.com/device?code=ABCD\","
               "\"interval\":7,\"expires_in\":1800}",
               &auth) == 0);
    EXPECT_STR_EQ(auth.device_code, "private-device-code");
    EXPECT_STR_EQ(auth.user_code, "ABCD-EFGH");
    EXPECT_STR_EQ(auth.verification_uri, "https://auth.kimi.com/device?code=ABCD");
    EXPECT(auth.interval == 7);
    EXPECT(auth.expires_in == 1800);
    kimi_device_auth_release(&auth);
    EXPECT(kimi_login_parse_device("{\"device_code\":\"d\",\"user_code\":\"c\","
                                   "\"verification_uri\":\"https://auth.kimi.com/device\"}",
                                   &auth) == 0);
    EXPECT(auth.interval == 5);
    EXPECT(auth.expires_in == 900);
    kimi_device_auth_release(&auth);
}

static void test_bad_device_responses(void)
{
    const char *bad[] = {
        NULL,
        "",
        "not json",
        "[]",
        "{}",
        "{\"device_code\":\"d\"}",
        "{\"device_code\":\"d\",\"user_code\":\"c\",\"verification_uri\":\"http://insecure/\"}",
        "{\"device_code\":\"d\",\"user_code\":\"c\",\"verification_uri\":\"https://x/"
        "\",\"interval\":-1}",
        "{\"device_code\":\"d\",\"user_code\":\"c\",\"verification_uri\":\"https://x/"
        "\",\"expires_in\":0}",
        "{\"device_code\":\"d\",\"user_code\":\"c\",\"verification_uri\":\"https://x/"
        "\",\"expires_in\":999999999999}",
        "{\"device_code\":\"d\",\"user_code\":\"c\\nINJECT\",\"verification_uri\":\"https://x/\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct kimi_device_auth auth;
        EXPECT(kimi_login_parse_device(bad[i], &auth) == -1);
        EXPECT(auth.device_code == NULL);
        EXPECT(auth.verification_uri == NULL);
    }
}

static void test_poll_states(void)
{
    EXPECT(kimi_login_classify_poll(200, TOKEN_RESPONSE) == KIMI_POLL_AUTHORIZED);
    EXPECT(kimi_login_classify_poll(400, "{\"error\":\"authorization_pending\"}") ==
           KIMI_POLL_PENDING);
    EXPECT(kimi_login_classify_poll(429, "{\"error\":\"slow_down\"}") == KIMI_POLL_SLOW_DOWN);
    EXPECT(kimi_login_classify_poll(400, "{\"error\":\"expired_token\"}") == KIMI_POLL_EXPIRED);
    EXPECT(kimi_login_classify_poll(403, "{\"error\":\"access_denied\"}") == KIMI_POLL_DENIED);
    EXPECT(kimi_login_classify_poll(500, "{\"error\":\"authorization_pending\"}") ==
           KIMI_POLL_FAILED);
    EXPECT(kimi_login_classify_poll(200, "{\"access_token\":\"only-access\"}") == KIMI_POLL_FAILED);
    EXPECT(kimi_login_classify_poll(403, "<html>denied</html>") == KIMI_POLL_FAILED);
    EXPECT(kimi_login_classify_poll(404, "{}") == KIMI_POLL_FAILED);
    EXPECT(kimi_login_classify_poll(0, NULL) == KIMI_POLL_FAILED);
}

static void test_token_and_refresh(void)
{
    time_t before = time(NULL);
    json_t *entry = kimi_login_parse_token(TOKEN_RESPONSE, NULL);
    EXPECT(entry != NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "access_token")), "access-secret");
    json_int_t expiry = json_integer_value(json_object_get(entry, "expires_at"));
    EXPECT(expiry >= (json_int_t)before + 3600);
    EXPECT(expiry <= (json_int_t)time(NULL) + 3600);
    json_object_set_new(entry, "device_id", json_string("device-1"));
    json_object_set_new(entry, "login_id", json_string("login-1"));
    EXPECT(kimi_login_entry_valid(entry));
    json_t *refreshed =
        kimi_login_parse_token("{\"access_token\":\"new-access\",\"expires_in\":7200}", entry);
    EXPECT(refreshed != NULL);
    EXPECT(json_integer_value(json_object_get(entry, "generation")) == 0);
    EXPECT(json_integer_value(json_object_get(refreshed, "generation")) == 1);
    EXPECT_STR_EQ(json_string_value(json_object_get(refreshed, "refresh_token")), "refresh-secret");
    EXPECT_STR_EQ(json_string_value(json_object_get(refreshed, "device_id")), "device-1");
    EXPECT_STR_EQ(json_string_value(json_object_get(refreshed, "login_id")), "login-1");
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "access_token")), "access-secret");
    json_decref(refreshed);
    refreshed = kimi_login_parse_token(
        "{\"access_token\":\"rotated-access\",\"refresh_token\":\"rotated-refresh\","
        "\"expires_in\":3600}",
        entry);
    EXPECT_STR_EQ(json_string_value(json_object_get(refreshed, "refresh_token")),
                  "rotated-refresh");
    json_decref(refreshed);
    EXPECT(kimi_login_parse_token(
               "{\"access_token\":\"at\",\"refresh_token\":\"\",\"expires_in\":3600}", entry) ==
           NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(entry, "refresh_token")), "refresh-secret");
    json_object_set_new(entry, "device_id", json_string("device\r\nInjected: yes"));
    EXPECT(!kimi_login_entry_valid(entry));
    json_decref(entry);
    const char *bad[] = {
        NULL,
        "{}",
        "not json",
        "[]",
        "{\"access_token\":\"at\",\"expires_in\":3600}",
        "{\"access_token\":\"at\",\"refresh_token\":\"rt\"}",
        "{\"access_token\":\"at\",\"refresh_token\":\"rt\",\"expires_in\":-1}",
        "{\"access_token\":\"at\",\"refresh_token\":\"rt\",\"expires_in\":\"3600\"}",
        "{\"access_token\":\"at\",\"refresh_token\":\"rt\",\"expires_in\":3600,\"token_type\":"
        "\"MAC\"}",
        "{\"access_token\":\"at\\r\\nInjected: "
        "yes\",\"refresh_token\":\"rt\",\"expires_in\":3600}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        EXPECT(kimi_login_parse_token(bad[i], NULL) == NULL);
}

static void test_refresh_rejection(void)
{
    EXPECT(kimi_login_refresh_rejected(400, "{\"error\":\"invalid_grant\"}"));
    EXPECT(kimi_login_refresh_rejected(401, "{\"error\":\"invalid_token\"}"));
    EXPECT(!kimi_login_refresh_rejected(500, "{\"error\":\"invalid_grant\"}"));
    EXPECT(!kimi_login_refresh_rejected(403, "<html>forbidden</html>"));
    EXPECT(!kimi_login_refresh_rejected(429, "{\"error\":\"rate_limit\"}"));
    EXPECT(!kimi_login_refresh_rejected(200, NULL));
}

static void test_form_encoding_and_trace(void)
{
    char *path = xasprintf("%s/trace", t_tempdir());
    config_set_override("trace", path);
    trace_init();
    char *body = kimi_login_grant_body("refresh_token", "secret+refresh/&=");
    EXPECT(strstr(body, "client_id=17e5f671-d194-4dfb-9706-5516cb48c098&") == body);
    EXPECT(strstr(body, "&grant_type=refresh_token&refresh_token=secret%2Brefresh%2F%26%3D") !=
           NULL);
    trace_request("POST", "https://auth.kimi.com/api/oauth/token", NULL, body, strlen(body));
    free(body);
    body = kimi_login_grant_body("urn:ietf:params:oauth:grant-type:device_code", "device+secret");
    EXPECT(strstr(body, "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&") !=
           NULL);
    EXPECT(strstr(body, "&device_code=device%2Bsecret") != NULL);
    trace_request("POST", "https://auth.kimi.com/api/oauth/token", NULL, body, strlen(body));
    free(body);
    char *contents = fs_read_file(path, NULL);
    EXPECT(contents != NULL);
    if (contents) {
        EXPECT(strstr(contents, "secret+refresh") == NULL);
        EXPECT(strstr(contents, "secret%2Brefresh") == NULL);
        EXPECT(strstr(contents, "device%2Bsecret") == NULL);
        EXPECT(strstr(contents, "<redacted>") != NULL);
        free(contents);
    }
    free(path);
}

static void test_login_store_and_logout(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    EXPECT(!kimi_login_present());
    EXPECT(kimi_login_status() == NULL);
    EXPECT(kimi_logout_run() == 0);
    json_t *entry = kimi_login_parse_token(TOKEN_RESPONSE, NULL);
    json_object_set_new(entry, "device_id", json_string("d"));
    json_object_set_new(entry, "login_id", json_string("l"));
    EXPECT(cred_store_set("kimi", entry) == 0);
    EXPECT(cred_store_set("codex", entry) == 0);
    json_decref(entry);
    EXPECT(kimi_login_present());
    char *status = kimi_login_status();
    EXPECT(status != NULL);
    free(status);
    char *path = cred_store_file_path();
    struct stat st;
    EXPECT(stat(path, &st) == 0);
    EXPECT((st.st_mode & 0777) == 0600);
    free(path);
    EXPECT(kimi_logout_run() == 1);
    EXPECT(!kimi_login_present());
    EXPECT(kimi_logout_run() == 0);
    entry = cred_store_get("codex");
    EXPECT(entry != NULL);
    json_decref(entry);
}

int main(void)
{
    test_device_response();
    test_bad_device_responses();
    test_poll_states();
    test_token_and_refresh();
    test_refresh_rejection();
    test_form_encoding_and_trace();
    test_login_store_and_logout();
    T_REPORT();
}
