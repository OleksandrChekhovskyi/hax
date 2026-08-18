/* SPDX-License-Identifier: MIT */
#include "providers/codex_auth.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "cred_store.h"
#include "util.h"
#include "system/path.h"
#include "text/base64.h"

#define CODEX_CLI_AUTH_PATH "~/.codex/auth.json"

json_t *codex_jwt_payload(const char *jwt)
{
    if (!jwt || !*jwt)
        return NULL;

    const char *payload_start = strchr(jwt, '.');
    if (!payload_start)
        return NULL;
    payload_start++;

    const char *payload_end = strchr(payload_start, '.');
    if (!payload_end)
        return NULL;

    unsigned char *payload =
        base64url_decode(payload_start, (size_t)(payload_end - payload_start), NULL);
    if (!payload)
        return NULL;

    json_t *root = json_loads((char *)payload, 0, NULL);
    free(payload);
    return root;
}

char *codex_jwt_email(const char *jwt)
{
    json_t *payload = codex_jwt_payload(jwt);
    if (!payload)
        return NULL;

    const char *email = json_string_value(json_object_get(payload, "email"));
    if (!email || !*email) {
        json_t *profile = json_object_get(payload, "https://api.openai.com/profile");
        email = json_string_value(json_object_get(profile, "email"));
    }

    char *result = email && *email ? xstrdup(email) : NULL;
    json_decref(payload);
    return result;
}

long codex_jwt_exp(const char *jwt)
{
    json_t *payload = codex_jwt_payload(jwt);
    if (!payload)
        return 0;

    json_t *exp = json_object_get(payload, "exp");
    long result = json_is_number(exp) ? (long)json_number_value(exp) : 0;
    json_decref(payload);
    return result > 0 ? result : 0;
}

char *codex_jwt_account_id(const char *jwt)
{
    json_t *payload = codex_jwt_payload(jwt);
    if (!payload)
        return NULL;

    json_t *auth_claim = json_object_get(payload, "https://api.openai.com/auth");
    const char *account_id = json_string_value(json_object_get(auth_claim, "chatgpt_account_id"));
    char *result = account_id && *account_id ? xstrdup(account_id) : NULL;
    json_decref(payload);
    return result;
}

enum codex_auth_status codex_auth_from_json(const json_t *root, struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));

    json_t *tokens = json_object_get(root, "tokens");
    const char *access_token = json_string_value(json_object_get(tokens, "access_token"));
    const char *account_id = json_string_value(json_object_get(tokens, "account_id"));
    if (!access_token || !*access_token || !account_id || !*account_id)
        return CODEX_AUTH_NO_TOKENS;

    auth->access_token = xstrdup(access_token);
    auth->account_id = xstrdup(account_id);
    auth->email = codex_jwt_email(json_string_value(json_object_get(tokens, "id_token")));
    auth->source = CODEX_AUTH_SOURCE_CODEX_CLI;
    return CODEX_AUTH_OK;
}

enum codex_auth_status codex_auth_from_store_entry(const json_t *entry, struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));

    const char *access_token = json_string_value(json_object_get(entry, "access_token"));
    const char *refresh_token = json_string_value(json_object_get(entry, "refresh_token"));
    const char *account_id = json_string_value(json_object_get(entry, "account_id"));
    if (!access_token || !*access_token || !refresh_token || !*refresh_token || !account_id ||
        !*account_id)
        return CODEX_AUTH_NO_TOKENS;

    auth->access_token = xstrdup(access_token);
    auth->refresh_token = xstrdup(refresh_token);
    auth->account_id = xstrdup(account_id);
    auth->email = codex_jwt_email(json_string_value(json_object_get(entry, "id_token")));
    auth->source = CODEX_AUTH_SOURCE_HAX;
    return CODEX_AUTH_OK;
}

static enum codex_auth_status load_codex_cli(struct codex_auth *auth, char **detail)
{
    char *path = path_expand_home(CODEX_CLI_AUTH_PATH);
    char *contents = slurp_file(path, NULL);
    if (!contents) {
        if (detail)
            *detail = path;
        else
            free(path);
        return CODEX_AUTH_NO_FILE;
    }
    free(path);

    json_error_t error;
    json_t *root = json_loads(contents, 0, &error);
    free(contents);
    if (!root) {
        if (detail)
            *detail = xstrdup(error.text);
        return CODEX_AUTH_BAD_JSON;
    }

    enum codex_auth_status status = codex_auth_from_json(root, auth);
    json_decref(root);
    return status;
}

enum codex_auth_status codex_auth_load(struct codex_auth *auth, char **detail)
{
    memset(auth, 0, sizeof(*auth));
    if (detail)
        *detail = NULL;

    json_t *entry = cred_store_get("codex");
    if (entry) {
        enum codex_auth_status status = codex_auth_from_store_entry(entry, auth);
        json_decref(entry);
        /* A partial entry falls through to the CLI rather than blocking it. */
        if (status == CODEX_AUTH_OK)
            return status;
    }

    return load_codex_cli(auth, detail);
}

static int same_string(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

int codex_auth_equal(const struct codex_auth *a, const struct codex_auth *b)
{
    return same_string(a->access_token, b->access_token) &&
           same_string(a->account_id, b->account_id);
}

const char *codex_auth_status_reason(enum codex_auth_status status)
{
    switch (status) {
    case CODEX_AUTH_OK:
        return NULL;
    case CODEX_AUTH_BAD_JSON:
        return "auth.json not valid JSON";
    case CODEX_AUTH_NO_FILE:
    case CODEX_AUTH_NO_TOKENS:
        return "not logged in (use /login)";
    }
    return "not logged in (use /login)";
}

void codex_auth_release(struct codex_auth *auth)
{
    free(auth->access_token);
    free(auth->account_id);
    free(auth->email);
    free(auth->refresh_token);
    memset(auth, 0, sizeof(*auth));
}
