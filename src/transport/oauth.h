/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSPORT_OAUTH_H
#define HAX_TRANSPORT_OAUTH_H

#include <stddef.h>

#include "transport/http.h"

/* OAuth authorization-code mechanics shared by provider login flows: PKCE material and a one-shot
 * loopback HTTP listener that catches the provider's browser redirect. Foreground-thread state. */

/* Generate an RFC 7636 PKCE pair: a base64url verifier from 64 random bytes and its S256
 * challenge. The caller owns both outputs. Aborts on entropy failure. */
void oauth_pkce_generate(char **verifier, char **challenge);

/* The S256 challenge for `verifier`: base64url(SHA-256(verifier)). The caller frees. */
char *oauth_pkce_challenge(const char *verifier);

/* A random base64url `state` token binding the redirect to this login attempt. The caller frees.
 * Aborts on entropy failure. */
char *oauth_state_generate(void);

struct oauth_listener;

/* Listen on the IPv4 loopback at the first bindable of `ports` (0 requests an ephemeral port),
 * plus the IPv6 loopback on the same port, since browsers may resolve `localhost` to ::1. A port
 * whose v6 side belongs to another service is skipped while a fully bindable candidate remains;
 * only then does v4-only coverage suffice. On success `*bound_port` receives the chosen port.
 * Returns NULL when no port binds. */
struct oauth_listener *oauth_listener_open(const int *ports, size_t n_ports, int *bound_port);

void oauth_listener_close(struct oauth_listener *listener);

enum oauth_redirect_result {
    OAUTH_REDIRECT_CODE,      /* the authorization code was captured */
    OAUTH_REDIRECT_DENIED,    /* the provider redirected back with an OAuth error */
    OAUTH_REDIRECT_TIMEOUT,   /* the deadline passed with no matching redirect */
    OAUTH_REDIRECT_CANCELLED, /* `tick` aborted the wait */
    OAUTH_REDIRECT_ERROR,     /* local socket failure */
};

/* Serve until a redirect to `path` carries a matching `state` plus a code or an OAuth error,
 * answering every request with a minimal HTML page on a closed connection. The state matches
 * alone or with a `.`-separated provider suffix (auth.openai.com appends onboarding context).
 * Other paths get 404, state mismatches 400, and stalled senders are dropped; all leave the
 * listener armed. On OAUTH_REDIRECT_CODE `*code_out` receives the owned, percent-decoded
 * authorization code; on OAUTH_REDIRECT_DENIED `*detail_out` an owned human-readable reason.
 * `tick` is polled throughout, including mid-request, so cancellation stays prompt. */
enum oauth_redirect_result oauth_listener_wait(struct oauth_listener *listener, const char *path,
                                               const char *state, long deadline_ms,
                                               http_tick_cb tick, void *tick_user, char **code_out,
                                               char **detail_out);

/* Pure request parsing, exposed for tests. */

/* Split an HTTP request line "METHOD /path?query HTTP/1.x" into an owned path and query (""
 * when absent). Returns 0 on success, -1 for anything else. */
int oauth_split_request_line(const char *request, char **path_out, char **query_out);

/* The percent-decoded value of `key` in a query string, with `+` as space. Invalid escapes pass
 * through literally. Returns NULL when the key is absent; the caller frees. */
char *oauth_query_param(const char *query, const char *key);

#endif /* HAX_TRANSPORT_OAUTH_H */
