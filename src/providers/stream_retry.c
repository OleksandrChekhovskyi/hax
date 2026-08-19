/* SPDX-License-Identifier: MIT */
#include "providers/stream_retry.h"

#include <stdlib.h>
#include <string.h>

#include "provider.h"
#include "util.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

int stream_retry_run(const struct stream_retry *request, stream_cb callback, void *callback_user,
                     http_tick_cb tick, void *tick_user)
{
    struct retry_policy policy = retry_policy_default();
    struct http_response response = {0};
    int parser_live = 0;
    int result = -1;

    for (int attempt = 0; attempt < policy.max_attempts; attempt++) {
        memset(&response, 0, sizeof(response));
        request->parser_init(request->ctx, callback, callback_user);
        parser_live = 1;

        char **headers = request->build_headers(request->ctx);
        result = http_sse_post(request->endpoint, (const char *const *)headers, request->body,
                               request->body_len, policy.idle_timeout_s, request->parser_feed,
                               request->ctx, tick, tick_user, &response);
        string_array_free(headers);

        if (!response.cancelled && request->recover &&
            request->recover(request->ctx, response.status, tick, tick_user)) {
            free(response.error_body);
            response.error_body = NULL;
            request->parser_free(request->ctx);
            parser_live = 0;
            attempt--;
            continue;
        }

        if (response.cancelled ||
            !retry_should_attempt(result, response.status, response.error_body) ||
            attempt + 1 >= policy.max_attempts) {
            break;
        }

        long delay_ms = response.retry_after_ms > 0 ? response.retry_after_ms
                                                    : retry_delay_ms(&policy, attempt);
        struct stream_event retry = {
            .kind = EV_RETRY,
            .u.retry =
                {
                    .attempt = attempt + 1,
                    .max_attempts = policy.max_attempts,
                    .delay_ms = delay_ms,
                    .http_status = (int)response.status,
                },
        };
        callback(&retry, callback_user);

        free(response.error_body);
        response.error_body = NULL;
        request->parser_free(request->ctx);
        parser_live = 0;

        if (retry_sleep_with_tick(delay_ms, tick, tick_user)) {
            response.cancelled = 1;
            break;
        }
    }

    if (!response.cancelled) {
        if (result != 0 || response.status < 200 || response.status >= 300) {
            char *message =
                request->error_message
                    ? request->error_message(request->ctx, response.status, response.error_body)
                    : NULL;
            if (!message)
                message = format_api_error(response.status, response.error_body);
            struct stream_event error = {
                .kind = EV_ERROR,
                .u.error = {.message = message, .http_status = (int)response.status},
            };
            callback(&error, callback_user);
            free(message);
        } else {
            request->parser_finalize(request->ctx);
        }
    }

    free(response.error_body);
    if (parser_live)
        request->parser_free(request->ctx);
    return result;
}
