/* SPDX-License-Identifier: MIT */
#include "providers/kimi_login.h"

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "busy.h"
#include "cred_store.h"
#include "trace.h"
#include "version.h"
#include "xalloc.h"
#include "system/clock.h"
#include "system/rand.h"
#include "terminal/ansi.h"
#include "terminal/clipboard.h"
#include "terminal/ui.h"
#include "text/url.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

/* Public client and form protocol used by MoonshotAI/kimi-cli's auth/oauth.py. */
#define KIMI_CLIENT_ID      "17e5f671-d194-4dfb-9706-5516cb48c098"
#define KIMI_AUTH_URL       "https://auth.kimi.com/api/oauth/"
#define KIMI_DEVICE_GRANT   "urn:ietf:params:oauth:grant-type:device_code"
#define KIMI_HTTP_TIMEOUT_S 30
#define KIMI_RESPONSE_MAX   (1024L * 1024L)

static const char *string_field(const json_t *root, const char *name)
{
    return json_string_value(json_object_get(root, name));
}

static int token_text_valid(const char *text)
{
    if (!text || !*text)
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 0x21 || *p > 0x7e)
            return 0;
    return 1;
}

int kimi_login_parse_device(const char *body, struct kimi_device_auth *out)
{
    memset(out, 0, sizeof(*out));
    json_t *root = body ? json_loads(body, 0, NULL) : NULL;
    const char *code = string_field(root, "device_code");
    const char *user_code = string_field(root, "user_code");
    const char *uri = string_field(root, "verification_uri_complete");
    if (!uri || !*uri)
        uri = string_field(root, "verification_uri");
    json_t *expiry = json_object_get(root, "expires_in");
    json_t *interval = json_object_get(root, "interval");
    json_int_t expires_in = expiry ? json_integer_value(expiry) : 900;
    json_int_t seconds = interval ? json_integer_value(interval) : 5;
    int valid = token_text_valid(code) && token_text_valid(user_code) && uri &&
                strncmp(uri, "https://", 8) == 0 && token_text_valid(uri) && expires_in > 0 &&
                expires_in <= 86400 && seconds > 0 && seconds <= 86400;
    if (valid) {
        out->device_code = xstrdup(code);
        out->user_code = xstrdup(user_code);
        out->verification_uri = xstrdup(uri);
        out->expires_in = (long)expires_in;
        out->interval = (long)seconds;
    }
    json_decref(root);
    return valid ? 0 : -1;
}

void kimi_device_auth_release(struct kimi_device_auth *auth)
{
    free(auth->device_code);
    free(auth->user_code);
    free(auth->verification_uri);
    memset(auth, 0, sizeof(*auth));
}

enum kimi_poll_result kimi_login_classify_poll(long status, const char *body)
{
    json_t *root = body ? json_loads(body, 0, NULL) : NULL;
    const char *error = string_field(root, "error");
    enum kimi_poll_result result = KIMI_POLL_FAILED;
    if (status >= 200 && status < 300) {
        json_t *entry = kimi_login_parse_token(body, NULL);
        if (entry)
            result = KIMI_POLL_AUTHORIZED;
        json_decref(entry);
    } else if (status >= 400 && status < 500 && error) {
        if (strcmp(error, "authorization_pending") == 0)
            result = KIMI_POLL_PENDING;
        else if (strcmp(error, "slow_down") == 0)
            result = KIMI_POLL_SLOW_DOWN;
        else if (strcmp(error, "expired_token") == 0)
            result = KIMI_POLL_EXPIRED;
        else if (strcmp(error, "access_denied") == 0)
            result = KIMI_POLL_DENIED;
    }
    json_decref(root);
    return result;
}

json_t *kimi_login_parse_token(const char *body, const json_t *previous)
{
    json_t *root = body ? json_loads(body, 0, NULL) : NULL;
    const char *access = string_field(root, "access_token");
    const char *refresh = string_field(root, "refresh_token");
    const char *type = string_field(root, "token_type");
    json_t *expiry = json_object_get(root, "expires_in");
    json_int_t seconds = json_integer_value(expiry);
    json_int_t generation = json_integer_value(json_object_get(previous, "generation"));
    if (!refresh)
        refresh = string_field(previous, "refresh_token");
    if (!token_text_valid(access) || !token_text_valid(refresh) ||
        (type && strcmp(type, "Bearer") != 0 && strcmp(type, "bearer") != 0) ||
        !json_is_integer(expiry) || seconds <= 0 || seconds > 31536000 || generation < 0 ||
        generation >= INT64_MAX - 1) {
        json_decref(root);
        return NULL;
    }
    json_t *entry = previous ? json_deep_copy(previous) : json_object();
    json_object_set_new(entry, "access_token", json_string(access));
    json_object_set_new(entry, "refresh_token", json_string(refresh));
    json_object_set_new(entry, "expires_at", json_integer((json_int_t)time(NULL) + seconds));
    json_object_set_new(entry, "generation", json_integer(previous ? generation + 1 : 0));
    json_decref(root);
    return entry;
}

int kimi_login_entry_valid(const json_t *entry)
{
    return token_text_valid(string_field(entry, "access_token")) &&
           token_text_valid(string_field(entry, "refresh_token")) &&
           token_text_valid(string_field(entry, "device_id")) &&
           token_text_valid(string_field(entry, "login_id")) &&
           json_is_integer(json_object_get(entry, "expires_at")) &&
           json_integer_value(json_object_get(entry, "expires_at")) > 0 &&
           json_is_integer(json_object_get(entry, "generation")) &&
           json_integer_value(json_object_get(entry, "generation")) >= 0 &&
           json_integer_value(json_object_get(entry, "generation")) < INT64_MAX;
}

int kimi_login_refresh_rejected(long status, const char *body)
{
    json_t *root = body ? json_loads(body, 0, NULL) : NULL;
    const char *error = string_field(root, "error");
    int rejected = status >= 400 && status < 500 && error &&
                   (strcmp(error, "invalid_grant") == 0 || strcmp(error, "invalid_token") == 0);
    json_decref(root);
    return rejected;
}

char *kimi_login_grant_body(const char *grant_type, const char *value)
{
    trace_register_secret(value);
    char *encoded = url_encode(value);
    trace_register_secret(encoded);
    char *grant = url_encode(grant_type);
    char *body = xasprintf(
        "client_id=" KIMI_CLIENT_ID "&grant_type=%s&%s=%s", grant,
        strcmp(grant_type, "refresh_token") == 0 ? "refresh_token" : "device_code", encoded);
    free(grant);
    free(encoded);
    return body;
}

static int oauth_post(const char *endpoint, const char *body, const char *device_id,
                      http_tick_cb tick, void *tick_user, char **response, long *status)
{
    char *device_header = xasprintf("X-Msh-Device-Id: %s", device_id);
    const char *headers[] = {"X-Msh-Platform: kimi_cli", "X-Msh-Version: " HAX_VERSION,
                             "User-Agent: hax/" HAX_VERSION, device_header, NULL};
    int result =
        http_post(endpoint, headers, "application/x-www-form-urlencoded", body, strlen(body),
                  KIMI_HTTP_TIMEOUT_S, KIMI_RESPONSE_MAX, tick, tick_user, response, status);
    free(device_header);
    return result;
}

int kimi_login_token_request(const char *body, const char *device_id, http_tick_cb tick,
                             void *tick_user, char **response, long *status)
{
    return oauth_post(KIMI_AUTH_URL "token", body, device_id, tick, tick_user, response, status);
}

int kimi_login_present(void)
{
    json_t *entry = cred_store_get("kimi");
    int present = kimi_login_entry_valid(entry);
    json_decref(entry);
    return present;
}

char *kimi_login_status(void)
{
    return kimi_login_present() ? xstrdup("logged in — hax manages this token") : NULL;
}

int kimi_logout_run(void)
{
    int result = cred_store_delete("kimi");
    if (result < 0)
        ui_error("cannot remove the Kimi login from the hax credential store");
    /* Kimi exposes no revocation endpoint; local logout must also work offline. */
    return result;
}

static int poll_device(const struct kimi_device_auth *auth, const char *device_id,
                       json_t **entry_out)
{
    char *body = kimi_login_grant_body(KIMI_DEVICE_GRANT, auth->device_code);
    long deadline = monotonic_ms() + auth->expires_in * 1000;
    long interval = auth->interval * 1000;
    struct busy *busy = busy_begin("waiting for Kimi browser approval...");
    const char *error = NULL;
    int cancelled = 0;
    while (!*entry_out && !error) {
        long remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            error = "Kimi login timed out — the device code expired";
            break;
        }
        if (retry_sleep_with_tick(interval < remaining ? interval : remaining, busy_tick, NULL)) {
            cancelled = 1;
            break;
        }
        if (monotonic_ms() >= deadline) {
            error = "Kimi login timed out — the device code expired";
            break;
        }
        char *response = NULL;
        long status = 0;
        int result = kimi_login_token_request(body, device_id, busy_tick, NULL, &response, &status);
        enum kimi_poll_result poll = kimi_login_classify_poll(status, response);
        if (result != 0)
            error = "cannot reach auth.kimi.com — check your network and try /login kimi again";
        else if (poll == KIMI_POLL_AUTHORIZED)
            *entry_out = kimi_login_parse_token(response, NULL);
        else if (poll == KIMI_POLL_SLOW_DOWN)
            interval += 5000;
        else if (poll == KIMI_POLL_EXPIRED)
            error = "Kimi device code expired — run /login kimi again";
        else if (poll == KIMI_POLL_DENIED)
            error = "Kimi login was denied";
        else if (poll != KIMI_POLL_PENDING)
            error = "Kimi returned an unusable device login response — run /login kimi again";
        free(response);
    }
    cancelled |= busy_end(busy);
    free(body);
    /* A successful token response wins over a cancellation queued behind it. */
    if (*entry_out)
        return 0;
    if (cancelled)
        return 1;
    ui_error("%s", error);
    return -1;
}

int kimi_login_run(void)
{
    char device_id[37];
    gen_uuid_v4(device_id);
    struct busy *busy = busy_begin("contacting auth.kimi.com...");
    char *response = NULL;
    long status = 0;
    int result = oauth_post(KIMI_AUTH_URL "device_authorization", "client_id=" KIMI_CLIENT_ID,
                            device_id, busy_tick, NULL, &response, &status);
    int cancelled = busy_end(busy);
    if (cancelled) {
        free(response);
        return 1;
    }
    if (result != 0 || status < 200 || status >= 300) {
        char *message = format_api_error(status, response);
        ui_error("Kimi device login request failed: %s", message);
        free(message);
        free(response);
        return -1;
    }
    struct kimi_device_auth auth;
    result = kimi_login_parse_device(response, &auth);
    free(response);
    if (result != 0) {
        ui_error("unrecognized device login response from auth.kimi.com");
        return -1;
    }
    trace_register_secret(auth.device_code);
    trace_register_secret(auth.user_code);
    trace_register_secret(auth.verification_uri);
    const char *clipboard_error = NULL;
    int copied = clipboard_copy(auth.user_code, strlen(auth.user_code), &clipboard_error) == 0;
    printf("  open\n" ANSI_BOLD "%s" ANSI_BOLD_OFF "\n", auth.verification_uri);
    printf("  and approve code " ANSI_BOLD "%s" ANSI_BOLD_OFF "%s\n\n", auth.user_code,
           copied ? " (copied to clipboard)" : "");
    fflush(stdout);
    json_t *entry = NULL;
    result = poll_device(&auth, device_id, &entry);
    kimi_device_auth_release(&auth);
    if (result != 0)
        return result;
    char login_id[37];
    gen_uuid_v4(login_id);
    json_object_set_new(entry, "device_id", json_string(device_id));
    json_object_set_new(entry, "login_id", json_string(login_id));
    trace_register_secret(string_field(entry, "access_token"));
    trace_register_secret(string_field(entry, "refresh_token"));
    result = cred_store_set("kimi", entry);
    json_decref(entry);
    if (result != 0) {
        ui_error("cannot save the Kimi login in the hax credential store");
        return -1;
    }
    ui_note("logged in to Kimi — hax now manages this token");
    return 0;
}
