/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_KIMI_LOGIN_H
#define HAX_PROVIDERS_KIMI_LOGIN_H

#include <jansson.h>

#include "transport/http.h"

/* Kimi Code's public device OAuth client. Credentials belong to hax, not the Kimi CLI. */
int kimi_login_run(void);  /* 0: saved, 1: cancelled, -1: reported failure */
int kimi_logout_run(void); /* 1: removed locally, 0: absent, -1: reported failure */
int kimi_login_present(void);
char *kimi_login_status(void); /* owned, or NULL when absent */

struct kimi_device_auth {
    char *device_code;
    char *user_code;
    char *verification_uri;
    long expires_in;
    long interval;
};

/* Pure protocol helpers. Successful parses return owned objects/fields. */
int kimi_login_parse_device(const char *body, struct kimi_device_auth *out);
void kimi_device_auth_release(struct kimi_device_auth *auth);

enum kimi_poll_result {
    KIMI_POLL_AUTHORIZED,
    KIMI_POLL_PENDING,
    KIMI_POLL_SLOW_DOWN,
    KIMI_POLL_EXPIRED,
    KIMI_POLL_DENIED,
    KIMI_POLL_FAILED,
};

enum kimi_poll_result kimi_login_classify_poll(long status, const char *body);
/* Requires Bearer tokens and a positive expires_in; refresh may omit refresh_token. */
json_t *kimi_login_parse_token(const char *body, const json_t *previous);
int kimi_login_entry_valid(const json_t *entry);
int kimi_login_refresh_rejected(long status, const char *body);
char *kimi_login_grant_body(const char *grant_type, const char *value);

/* POST a device/refresh grant to Kimi's pinned token endpoint. Returns like http_post. */
int kimi_login_token_request(const char *body, const char *device_id, http_tick_cb tick,
                             void *tick_user, char **response, long *status);

#endif /* HAX_PROVIDERS_KIMI_LOGIN_H */
