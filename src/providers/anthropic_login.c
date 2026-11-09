/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_login.h"

#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "busy.h"
#include "cred_store.h"
#include "trace.h"
#include "xalloc.h"
#include "system/browser.h"
#include "system/rand.h"
#include "terminal/input.h"
#include "terminal/ui.h"
#include "text/url.h"
#include "transport/http.h"
#include "transport/oauth.h"

#define ANTHROPIC_CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define ANTHROPIC_REDIRECT  "https://console.anthropic.com/oauth/code/callback"
#define ANTHROPIC_TOKEN_URL "https://console.anthropic.com/v1/oauth/token"

static const char *string_field(const json_t *entry, const char *key)
{
    return json_string_value(json_object_get(entry, key));
}

static int token_valid(const char *text)
{
    if (!text || !*text)
        return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 0x21 || *p > 0x7e)
            return 0;
    return 1;
}

char *anthropic_login_authorize_url(const char *challenge, const char *state)
{
    char *encoded_challenge = url_encode(challenge);
    char *encoded_state = url_encode(state);
    char *url =
        xasprintf("https://claude.ai/oauth/authorize?code=true&client_id=" ANTHROPIC_CLIENT_ID
                  "&response_type=code&redirect_uri="
                  "https%%3A%%2F%%2Fconsole.anthropic.com%%2Foauth%%2Fcode%%2Fcallback"
                  "&scope=org%%3Acreate_api_key%%20user%%3Aprofile%%20user%%3Ainference"
                  "&code_challenge=%s&code_challenge_method=S256&state=%s",
                  encoded_challenge, encoded_state);
    free(encoded_challenge);
    free(encoded_state);
    return url;
}

char *anthropic_login_code_body(const char *input, const char *verifier, const char *state)
{
    if (!input || !verifier || !state)
        return NULL;
    const char *separator = strchr(input, '#');
    if (!separator || separator == input || strcmp(separator + 1, state) != 0)
        return NULL;
    char *code = xstrdup(input);
    code[separator - input] = '\0';
    if (!token_valid(code)) {
        free(code);
        return NULL;
    }
    trace_register_secret(code);
    trace_register_secret(verifier);
    trace_register_secret(state);
    json_t *root = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "grant_type", "authorization_code",
                             "client_id", ANTHROPIC_CLIENT_ID, "redirect_uri", ANTHROPIC_REDIRECT,
                             "code", code, "state", state, "code_verifier", verifier);
    char *body = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    free(code);
    return body;
}

char *anthropic_login_refresh_body(const char *refresh_token)
{
    trace_register_secret(refresh_token);
    json_t *root = json_pack("{s:s,s:s,s:s}", "grant_type", "refresh_token", "client_id",
                             ANTHROPIC_CLIENT_ID, "refresh_token", refresh_token);
    char *body = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return body;
}

json_t *anthropic_login_parse_token(const char *body, const json_t *previous)
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
    if (!token_valid(access) || !token_valid(refresh) ||
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

int anthropic_login_entry_valid(const json_t *entry)
{
    return token_valid(string_field(entry, "access_token")) &&
           token_valid(string_field(entry, "refresh_token")) &&
           token_valid(string_field(entry, "login_id")) &&
           json_is_integer(json_object_get(entry, "expires_at")) &&
           json_integer_value(json_object_get(entry, "expires_at")) > 0 &&
           json_is_integer(json_object_get(entry, "generation")) &&
           json_integer_value(json_object_get(entry, "generation")) >= 0 &&
           json_integer_value(json_object_get(entry, "generation")) < INT64_MAX;
}

int anthropic_login_refresh_rejected(long status, const char *body)
{
    json_t *root = body ? json_loads(body, 0, NULL) : NULL;
    const char *error = string_field(root, "error");
    int rejected = status >= 400 && status < 500 && error &&
                   (strcmp(error, "invalid_grant") == 0 || strcmp(error, "invalid_token") == 0);
    json_decref(root);
    return rejected;
}

int anthropic_login_token_request(const char *body, http_tick_cb tick, void *tick_user,
                                  char **response, long *status)
{
    return http_post(ANTHROPIC_TOKEN_URL, NULL, "application/json", body, strlen(body), 30,
                     1024L * 1024L, tick, tick_user, response, status);
}

int anthropic_login_present(void)
{
    json_t *entry = cred_store_get("anthropic");
    int present = anthropic_login_entry_valid(entry);
    json_decref(entry);
    return present;
}

char *anthropic_login_status(void)
{
    return anthropic_login_present() ? xstrdup("logged in — hax manages this token") : NULL;
}

int anthropic_logout_run(void)
{
    int result = cred_store_delete("anthropic");
    if (result < 0)
        ui_error("cannot remove the Anthropic login from the hax credential store");
    return result;
}

int anthropic_login_run(void)
{
    char *verifier = NULL;
    char *challenge = NULL;
    oauth_pkce_generate(&verifier, &challenge);
    char *state = oauth_state_generate();
    char *url = anthropic_login_authorize_url(challenge, state);
    browser_open_url(url);
    printf("  approve the login in your browser, then paste the code#state shown there\n%s\n\n",
           url);
    fflush(stdout);
    struct input *input = input_new();
    char *code = input_readline(input, "authorization code: ");
    input_free(input);
    char *body = anthropic_login_code_body(code, verifier, state);
    int result = -1;
    if (!code) {
        result = 1;
        goto out;
    }
    if (!body) {
        ui_error("invalid authorization code or state mismatch — run /login anthropic again");
        goto out;
    }
    struct busy *busy = busy_begin("exchanging Anthropic authorization code...");
    char *response = NULL;
    long status = 0;
    int request = anthropic_login_token_request(body, busy_tick, NULL, &response, &status);
    int cancelled = busy_end(busy);
    json_t *entry = request == 0 && status >= 200 && status < 300
                        ? anthropic_login_parse_token(response, NULL)
                        : NULL;
    free(response);
    if (!entry) {
        if (cancelled)
            result = 1;
        else
            ui_error("Anthropic login failed (HTTP %ld) — run /login anthropic again", status);
        goto out;
    }
    char login_id[37];
    gen_uuid_v4(login_id);
    json_object_set_new(entry, "login_id", json_string(login_id));
    trace_register_secret(string_field(entry, "access_token"));
    trace_register_secret(string_field(entry, "refresh_token"));
    result = cred_store_set("anthropic", entry);
    json_decref(entry);
    if (result != 0)
        ui_error("cannot save the Anthropic login in the hax credential store");
    else
        ui_note("logged in to Anthropic — hax now manages this token");
out:
    free(body);
    free(code);
    free(url);
    free(state);
    free(challenge);
    free(verifier);
    return result;
}
