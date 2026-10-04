/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_LOOPBACK_H
#define HAX_TESTS_LOOPBACK_H

#include <pthread.h>
#include <stdatomic.h>

/* A loopback HTTP server for tests. One background thread serves a scripted reply to each of
 * `n_requests` sequential connections and keeps the request it read. Zero-initialize, script the
 * replies, then loopback_start (or loopback_listen and loopback_serve separately when the test must
 * act between binding and accepting). Every reply must carry "Connection: close" so the client
 * reconnects for the next one; the thread exits after the last reply, or after ten seconds without
 * a client so a scenario fails instead of hanging. Set `hold` to keep each reply back until
 * loopback_release, so a test can act while a request is provably in flight. */

#define LOOPBACK_MAX_REQUESTS     9
#define LOOPBACK_REQUEST_CAPACITY 8192

struct loopback {
    const char *response;                         /* reply to any request without its own */
    const char *responses[LOOPBACK_MAX_REQUESTS]; /* per-request replies, full HTTP text */
    int n_requests;                               /* connections to serve; 0 means one */
    int delay_ms;                                 /* pause between reading a request and replying */
    int hold;                                     /* reply only after loopback_release */
    char requests[LOOPBACK_MAX_REQUESTS][LOOPBACK_REQUEST_CAPACITY]; /* headers and body */
    _Atomic int accepted;                                            /* connections accepted */
    _Atomic int served;   /* replies fully written and closed */
    _Atomic int released; /* set by loopback_release */

    /* Internal to the server. */
    int listener_fd;
    pthread_t thread;
    int serving;
    char *owned[LOOPBACK_MAX_REQUESTS]; /* loopback_reply_ok allocations */
};

/* Bind an ephemeral loopback port without accepting yet. Returns the port, or -1. */
int loopback_listen(struct loopback *server);

/* Start serving on the bound listener. Returns 0, or -1 with the listener closed. */
int loopback_serve(struct loopback *server);

/* Bind and serve. Returns the port, or -1. */
int loopback_start(struct loopback *server);

/* Let a held server send its replies. */
void loopback_release(struct loopback *server);

/* Wait for the server thread to finish, close the listener, and release scripted replies. */
void loopback_stop(struct loopback *server);

/* Script reply `index` as a 200 response carrying `body` with its Content-Length. The text is owned
 * by the server until loopback_stop. */
void loopback_reply_ok(struct loopback *server, int index, const char *body);

#endif /* HAX_TESTS_LOOPBACK_H */
