/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_STREAM_RETRY_H
#define HAX_PROVIDERS_STREAM_RETRY_H

#include <stddef.h>

#include "provider.h"
#include "transport/http.h"
#include "transport/sse.h"

/* Drives one provider stream() call to completion: http_sse_post attempts under the shared
 * retry policy, EV_RETRY between attempts, and a terminal EV_ERROR or a finalized parser.
 * Adapters supply the request bytes and the protocol hooks; the loop stays protocol-agnostic. */
struct stream_retry {
    const char *endpoint;
    const char *body; /* resent unchanged on every attempt */
    size_t body_len;
    void *ctx; /* borrowed; passed to every hook */
    /* Return owned NULL-terminated headers, freed after the attempt. Called per attempt
     * because credentials may rotate between attempts. */
    char **(*build_headers)(void *ctx);
    /* Establish fresh parser state; every attempt feeds a fresh parser. */
    void (*parser_init)(void *ctx, stream_cb callback, void *callback_user);
    sse_cb parser_feed; /* receives `ctx` as its user pointer */
    /* Emit the parser's terminal events after a completed 2xx stream. */
    void (*parser_finalize)(void *ctx);
    /* Release parser state; called between attempts and once on exit. */
    void (*parser_free)(void *ctx);
    /* Optional. Runs after every non-cancelled attempt; return non-zero to redo the attempt
     * immediately without consuming a retry. The hook must bound its own recoveries or the
     * loop never terminates. */
    int (*recover)(void *ctx, long http_status, http_tick_cb tick, void *tick_user);
    /* Optional. Return an allocated terminal-error message, or NULL to fall back to
     * format_api_error. */
    char *(*error_message)(void *ctx, long http_status, const char *error_body);
};

/* Returns the last attempt's http_sse_post result. Emits nothing after cancellation. */
int stream_retry_run(const struct stream_retry *request, stream_cb callback, void *callback_user,
                     http_tick_cb tick, void *tick_user);

#endif /* HAX_PROVIDERS_STREAM_RETRY_H */
