/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_AUTH_H
#define HAX_PROVIDERS_VERTEX_AUTH_H

/* Google OAuth2 access token state for the Vertex provider. hax runs a single active provider,
 * so the token lives in this module. */

/* Return whether a credential source appears configured. Fills `reason` (may be NULL) with a
 * static sentence when unavailable. */
int vertex_auth_available(const char **reason);

/* Return the process-wide bearer token, resolving from static key or gcloud user ADC and
 * refreshing as needed. The token is borrowed and stays valid until the next call. Returns
 * NULL with an allocated *error on failure. */
const char *vertex_auth_token(char **error);

/* Drop any cached token so the next vertex_auth_token re-resolves it (e.g. on a rejected
 * request). */
void vertex_auth_invalidate(void);

#endif /* HAX_PROVIDERS_VERTEX_AUTH_H */
