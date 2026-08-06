/* SPDX-License-Identifier: MIT */
#include "providers/codex_auth.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "system/path.h"
#include "text/base64.h"

#define CODEX_AUTH_PATH "~/.codex/auth.json"

char *codex_jwt_email(const char *jwt)
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
    if (!root)
        return NULL;

    const char *email = json_string_value(json_object_get(root, "email"));
    if (!email || !*email) {
        json_t *profile = json_object_get(root, "https://api.openai.com/profile");
        email = json_string_value(json_object_get(profile, "email"));
    }

    char *result = email && *email ? xstrdup(email) : NULL;
    json_decref(root);
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
    return CODEX_AUTH_OK;
}

enum codex_auth_status codex_auth_load(struct codex_auth *auth, char **detail)
{
    memset(auth, 0, sizeof(*auth));
    if (detail)
        *detail = NULL;

    char *path = path_expand_home(CODEX_AUTH_PATH);
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
        return "codex CLI not logged in";
    }
    return "codex CLI not logged in";
}

void codex_auth_release(struct codex_auth *auth)
{
    free(auth->access_token);
    free(auth->account_id);
    free(auth->email);
    memset(auth, 0, sizeof(*auth));
}
