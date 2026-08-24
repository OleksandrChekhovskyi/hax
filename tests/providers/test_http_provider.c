/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "catalog.h"
#include "config.h"
#include "effort.h"
#include "harness.h"
#include "provider.h"
#include "util.h"
#include "providers/http_provider.h"
#include "providers/wire.h"

static void test_list_efforts_wiring(void)
{
    struct http_provider_preset with_efforts = {
        .display_name = "with-efforts",
        .default_base_url = "http://example.invalid/v1",
        .efforts = EFFORT_LADDER,
        .n_efforts = EFFORT_LADDER_N,
    };
    struct provider *provider = http_provider_new_preset(&with_efforts);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT(provider->list_models != NULL);
        EXPECT(provider->list_efforts != NULL);
        const char *const *efforts = NULL;
        size_t n_efforts = provider->list_efforts(provider, &efforts);
        EXPECT(n_efforts == EFFORT_LADDER_N);
        EXPECT(efforts != NULL && strcmp(efforts[0], "none") == 0);
        EXPECT(strcmp(efforts[n_efforts - 1], "max") == 0);
        provider->destroy(provider);
    }

    struct http_provider_preset without_efforts = {
        .display_name = "without-efforts",
        .default_base_url = "http://example.invalid/v1",
    };
    provider = http_provider_new_preset(&without_efforts);
    EXPECT(provider != NULL);
    if (provider) {
        const char *const *efforts = NULL;
        EXPECT(provider->list_efforts(provider, &efforts) == 0);
        provider->destroy(provider);
    }
}

/* On the Messages wire the effort ladder steers adaptive thinking, so only an explicit
 * budget/off pin hides it: an unconfigured budget default upgrades when an effort is chosen. */
static void test_messages_efforts_follow_thinking_mode(void)
{
    struct http_provider_preset preset = {
        .display_name = "x",
        .default_base_url = "http://example.invalid/v1",
        .config_prefix = "providers.x",
        .wire = &WIRE_ANTHROPIC_MESSAGES,
        .efforts = EFFORT_LADDER,
        .n_efforts = EFFORT_LADDER_N,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        /* A pure Messages provider drops "minimal" (no Messages meaning) but keeps "none"
         * (thinking off). */
        const char *const *efforts = NULL;
        EXPECT(provider->list_efforts(provider, &efforts) == EFFORT_LADDER_N - 1);
        EXPECT(efforts != NULL && strcmp(efforts[0], "none") == 0);
        EXPECT(strcmp(efforts[1], "low") == 0);

        EXPECT(config_load("{\"providers\": {\"x\": {\"thinking_mode\": \"budget\"}}}") == 0);
        EXPECT(provider->list_efforts(provider, &efforts) == 0);

        EXPECT(config_load("{\"providers\": {\"x\": {\"thinking_mode\": \"adaptive\"}}}") == 0);
        EXPECT(provider->list_efforts(provider, &efforts) == EFFORT_LADDER_N - 1);

        /* A typo is not a pin: requests fall back to the default, so the ladder stays. */
        EXPECT(config_load("{\"providers\": {\"x\": {\"thinking_mode\": \"bugdet\"}}}") == 0);
        EXPECT(provider->list_efforts(provider, &efforts) == EFFORT_LADDER_N - 1);
        EXPECT(config_load(NULL) == 0);
        provider->destroy(provider);
    }

    /* On a mixed provider a budget pin affects only the Messages models, so the ladder must
     * survive for the models routed to other wires. */
    preset.catalog_wires = 1;
    provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        const char *const *efforts = NULL;
        EXPECT(config_load("{\"providers\": {\"x\": {\"thinking_mode\": \"budget\"}}}") == 0);
        EXPECT(provider->list_efforts(provider, &efforts) == EFFORT_LADDER_N);
        EXPECT(config_load(NULL) == 0);
        provider->destroy(provider);
    }
}

static int headers_have_version(char **headers)
{
    for (char **header = headers; header && *header; header++)
        if (strncmp(*header, "anthropic-version:", 18) == 0)
            return 1;
    return 0;
}

/* The version header marks the Messages wire, so it observes which family a provider landed
 * on: <prefix>.api must not move a provider across wire families in either direction. */
static void test_api_override_stays_in_family(void)
{
    EXPECT(config_load("{\"providers\": {\"x\": {\"api\": \"anthropic-messages\"}}}") == 0);
    struct http_provider_preset preset = {
        .display_name = "x",
        .default_base_url = "http://example.invalid/v1",
        .config_prefix = "providers.x",
    };
    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        char **headers = http_provider_metadata_headers(provider);
        EXPECT(!headers_have_version(headers));
        string_array_free(headers);
        provider->destroy(provider);
    }

    EXPECT(config_load("{\"providers\": {\"x\": {\"api\": \"responses\"}}}") == 0);
    preset.wire = &WIRE_ANTHROPIC_MESSAGES;
    provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        char **headers = http_provider_metadata_headers(provider);
        EXPECT(headers_have_version(headers));
        string_array_free(headers);
        provider->destroy(provider);
    }
    EXPECT(config_load(NULL) == 0);
}

/* The /models dialect and its auth scheme follow metadata_api, not the request wire: a Messages
 * endpoint can front an OpenAI-shaped catalog and vice versa. The version header marks the
 * Anthropic side; the probe hook exists only there. */
static void test_metadata_api_override(void)
{
    EXPECT(config_load("{\"providers\": {\"x\": {\"metadata_api\": \"openai\"}}}") == 0);
    struct http_provider_preset preset = {
        .display_name = "x",
        .default_base_url = "http://example.invalid/v1",
        .config_prefix = "providers.x",
        .wire = &WIRE_ANTHROPIC_MESSAGES,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        char **headers = http_provider_metadata_headers(provider);
        EXPECT(!headers_have_version(headers));
        EXPECT(provider->probe_model == NULL);
        string_array_free(headers);
        provider->destroy(provider);
    }

    EXPECT(config_load("{\"providers\": {\"x\": {\"metadata_api\": \"anthropic\"}}}") == 0);
    preset.wire = &WIRE_OPENAI_CHAT;
    provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        char **headers = http_provider_metadata_headers(provider);
        EXPECT(headers_have_version(headers));
        EXPECT(provider->probe_model != NULL);
        string_array_free(headers);
        provider->destroy(provider);
    }
    EXPECT(config_load(NULL) == 0);
}

#define MAX_REQUESTS 8

/* Serves one canned response per sequential connection, capturing each request. */
struct wire_server {
    int listener_fd;
    const char *response;
    int n_requests;
    char requests[MAX_REQUESTS][8192];
    _Atomic int served;
};

static void *serve_requests(void *user)
{
    struct wire_server *server = user;
    for (int i = 0; i < server->n_requests; i++) {
        struct pollfd poll_fd = {.fd = server->listener_fd, .events = POLLIN};
        if (poll(&poll_fd, 1, 10000) <= 0)
            return NULL;
        int client_fd = accept(server->listener_fd, NULL, NULL);
        if (client_fd < 0)
            return NULL;

        char *request = server->requests[i];
        size_t request_len = 0;
        size_t expected_len = 0;
        while (request_len < sizeof(server->requests[i]) - 1) {
            ssize_t bytes_read = read(client_fd, request + request_len,
                                      sizeof(server->requests[i]) - request_len - 1);
            if (bytes_read <= 0)
                break;
            request_len += (size_t)bytes_read;
            request[request_len] = '\0';

            char *header_end = strstr(request, "\r\n\r\n");
            if (header_end && expected_len == 0) {
                const char *length = strstr(request, "Content-Length: ");
                expected_len = (size_t)(header_end + 4 - request) +
                               (length ? strtoul(length + 16, NULL, 10) : 0);
            }
            if (expected_len > 0 && request_len >= expected_len)
                break;
        }

        size_t response_len = strlen(server->response);
        size_t written = 0;
        while (written < response_len) {
            ssize_t result = write(client_fd, server->response + written, response_len - written);
            if (result <= 0)
                break;
            written += (size_t)result;
        }
        close(client_fd);
        atomic_fetch_add(&server->served, 1);
    }
    return NULL;
}

static int start_server(struct wire_server *server, pthread_t *thread)
{
    server->listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener_fd < 0)
        return -1;

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server->listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listener_fd, MAX_REQUESTS) != 0)
        goto error;

    socklen_t address_len = sizeof(address);
    if (getsockname(server->listener_fd, (struct sockaddr *)&address, &address_len) != 0)
        goto error;
    if (pthread_create(thread, NULL, serve_requests, server) != 0)
        goto error;
    return ntohs(address.sin_port);

error:
    close(server->listener_fd);
    server->listener_fd = -1;
    return -1;
}

struct error_log {
    int n_errors;
    char message[256];
};

static int log_error(const struct stream_event *event, void *user)
{
    struct error_log *log = user;
    if (event->kind == EV_ERROR) {
        log->n_errors++;
        snprintf(log->message, sizeof(log->message), "%s", event->u.error.message);
    }
    return 0;
}

/* Point the catalog cache tier at a private snapshot naming each model's wire. */
static void write_catalog_fixture(void)
{
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[512];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        FAIL("fopen %s: %s", path, strerror(errno));
    fputs("{\"zen-test\": {\"npm\": \"@ai-sdk/openai-compatible\", \"models\": {"
          "\"claude-hint\": {\"provider\": {\"npm\": \"@ai-sdk/anthropic\"}},"
          "\"gemini-hint\": {\"provider\": {\"npm\": \"@ai-sdk/google\"}},"
          "\"think-hint\": {\"interleaved\": {\"field\": \"reasoning_content\"}}}}}",
          f);
    fclose(f);
}

/* One provider, one endpoint, three wires: model_apis rules and catalog hints pick each
 * request's protocol, path, and auth scheme; unmatched models keep the default wire. */
static void test_model_wire_routing(void)
{
    write_catalog_fixture();
    struct wire_server server = {
        .response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno",
        .n_requests = 7,
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    EXPECT(config_load("{\"providers\": {\"zen\": {\"api_key\": \"sk-test\","
                       " \"model_apis\": {\"claude-rule-*\": \"anthropic-messages\","
                       " \"r-*\": \"openai-responses\"}}},"
                       " \"catalog\": {\"models\": {\"zen-test\": {"
                       " \"cfg-pin\": {\"api\": \"anthropic-messages\"}}}}}") == 0);
    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    struct http_provider_preset preset = {
        .display_name = "zen",
        .default_base_url = base_url,
        .config_prefix = "providers.zen",
        .catalog_id = "zen-test",
        .catalog_wires = 1,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (!provider)
        return;

    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    struct error_log log = {0};
    provider->stream(provider, &context, "claude-rule-1", log_error, &log, NULL, NULL);
    provider->stream(provider, &context, "r-1", log_error, &log, NULL, NULL);
    provider->stream(provider, &context, "claude-hint", log_error, &log, NULL, NULL);
    provider->stream(provider, &context, "plain", log_error, &log, NULL, NULL);
    context.effort = "high";
    provider->stream(provider, &context, "claude-hint", log_error, &log, NULL, NULL);
    context.effort = NULL;
    provider->stream(provider, &context, "cfg-pin", log_error, &log, NULL, NULL);
    context.effort = "minimal";
    provider->stream(provider, &context, "claude-rule-1", log_error, &log, NULL, NULL);
    context.effort = NULL;
    pthread_join(thread, NULL);
    close(server.listener_fd);
    EXPECT(atomic_load(&server.served) == 7);

    EXPECT(strncmp(server.requests[0], "POST /messages HTTP", 19) == 0);
    EXPECT(strstr(server.requests[0], "x-api-key: sk-test\r\n") != NULL);
    EXPECT(strstr(server.requests[0], "anthropic-version: 2023-06-01\r\n") != NULL);
    EXPECT(strstr(server.requests[0], "\"max_tokens\"") != NULL);
    /* No effort selected: the compat-safe budget default. */
    EXPECT(strstr(server.requests[0], "\"budget_tokens\"") != NULL);

    EXPECT(strncmp(server.requests[1], "POST /responses HTTP", 20) == 0);
    EXPECT(strstr(server.requests[1], "Authorization: Bearer sk-test\r\n") != NULL);

    EXPECT(strncmp(server.requests[2], "POST /messages HTTP", 19) == 0);

    EXPECT(strncmp(server.requests[3], "POST /chat/completions HTTP", 27) == 0);
    EXPECT(strstr(server.requests[3], "Authorization: Bearer sk-test\r\n") != NULL);

    /* A selected effort upgrades the unconfigured default to adaptive thinking. */
    EXPECT(strncmp(server.requests[4], "POST /messages HTTP", 19) == 0);
    EXPECT(strstr(server.requests[4], "\"adaptive\"") != NULL);
    EXPECT(strstr(server.requests[4], "\"effort\":\"high\"") != NULL);
    EXPECT(strstr(server.requests[4], "\"budget_tokens\"") == NULL);

    /* A catalog.models api pin routes like a snapshot hint. */
    EXPECT(strncmp(server.requests[5], "POST /messages HTTP", 19) == 0);

    /* "minimal" can reach a rule-routed Messages model on a mixed provider; the wire cannot
     * spell it, so the request clamps to the Messages minimum instead. */
    EXPECT(strncmp(server.requests[6], "POST /messages HTTP", 19) == 0);
    EXPECT(strstr(server.requests[6], "\"adaptive\"") != NULL);
    EXPECT(strstr(server.requests[6], "\"effort\":\"low\"") != NULL);
    EXPECT(strstr(server.requests[6], "\"minimal\"") == NULL);

    EXPECT(log.n_errors == 7); /* every canned reply is a 400 */
    provider->destroy(provider);
    EXPECT(config_load(NULL) == 0);
}

/* The def-declared Messages defaults reach the request: adaptive thinking (not a compat-safe
 * budget) and prompt-cache markers on. Effort "none" has no Messages spelling, so it disables
 * thinking instead of riding output_config. */
static void test_messages_defaults_follow_preset(void)
{
    struct wire_server server = {
        .response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno",
        .n_requests = 2,
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    struct http_provider_preset preset = {
        .display_name = "fp",
        .default_base_url = base_url,
        .config_prefix = "providers.fp",
        .wire = &WIRE_ANTHROPIC_MESSAGES,
        .cache = "on",
        .thinking_mode = "adaptive",
        .strict_signatures = 1,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (!provider)
        return;

    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    struct error_log log = {0};
    provider->stream(provider, &context, "claude-x", log_error, &log, NULL, NULL);
    context.effort = "none";
    provider->stream(provider, &context, "claude-x", log_error, &log, NULL, NULL);
    pthread_join(thread, NULL);
    close(server.listener_fd);
    EXPECT(atomic_load(&server.served) == 2);

    EXPECT(strstr(server.requests[0], "\"adaptive\"") != NULL);
    EXPECT(strstr(server.requests[0], "\"budget_tokens\"") == NULL);
    EXPECT(strstr(server.requests[0], "\"cache_control\"") != NULL);

    EXPECT(strstr(server.requests[1], "\"thinking\"") == NULL);
    EXPECT(strstr(server.requests[1], "\"output_config\"") == NULL);
    provider->destroy(provider);
}

static void stream_one_reasoned_turn(struct provider *provider, char *model, struct error_log *log)
{
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = "hello"},
        {.kind = ITEM_REASONING, .reasoning_text = "thought", .provider = "zen", .model = model},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "hi"},
    };
    struct context context = {.items = items, .n_items = 3};
    provider->stream(provider, &context, model, log_error, log, NULL, NULL);
}

/* The catalog names the member per model; reasoning_roundtrip pins one for every model instead,
 * including an "off" the hint must not resurrect and an "auto" that asks for the hint back. */
static void test_interleaved_reasoning_replay(void)
{
    write_catalog_fixture();
    catalog_shutdown(); /* drop lookups memoized against an earlier fixture */
    struct wire_server server = {
        .response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno",
        .n_requests = 5,
    };
    pthread_t thread;
    int port = start_server(&server, &thread);
    EXPECT(port > 0);
    if (port <= 0)
        return;

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    struct http_provider_preset preset = {
        .display_name = "zen",
        .default_base_url = base_url,
        .config_prefix = "providers.zen",
        .catalog_id = "zen-test",
    };
    struct error_log log = {0};

    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        stream_one_reasoned_turn(provider, "think-hint", &log);
        stream_one_reasoned_turn(provider, "plain", &log);
        provider->destroy(provider);
    }

    EXPECT(config_load("{\"providers\": {\"zen\": {\"reasoning_roundtrip\": \"reasoning\"}}}") ==
           0);
    provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        stream_one_reasoned_turn(provider, "think-hint", &log);
        provider->destroy(provider);
    }

    EXPECT(config_load("{\"providers\": {\"zen\": {\"reasoning_roundtrip\": \"off\"}}}") == 0);
    provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        stream_one_reasoned_turn(provider, "think-hint", &log);
        provider->destroy(provider);
    }

    EXPECT(config_load("{\"providers\": {\"zen\": {\"reasoning_roundtrip\": \"auto\"}}}") == 0);
    provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        stream_one_reasoned_turn(provider, "think-hint", &log);
        provider->destroy(provider);
    }

    pthread_join(thread, NULL);
    close(server.listener_fd);
    EXPECT(atomic_load(&server.served) == 5);

    EXPECT(strstr(server.requests[0], "\"reasoning_content\":\"thought\"") != NULL);
    EXPECT(strstr(server.requests[1], "thought") == NULL);
    EXPECT(strstr(server.requests[2], "\"reasoning\":\"thought\"") != NULL);
    EXPECT(strstr(server.requests[3], "thought") == NULL);
    /* "auto" names the default resolution, not a member called "auto". */
    EXPECT(strstr(server.requests[4], "\"reasoning_content\":\"thought\"") != NULL);
    EXPECT(strstr(server.requests[4], "\"auto\"") == NULL);

    EXPECT(config_load(NULL) == 0);
}

/* A catalog hint naming an unimplemented protocol fails cleanly before any request. */
static void test_unsupported_protocol_reported(void)
{
    struct http_provider_preset preset = {
        .display_name = "zen",
        .default_base_url = "http://127.0.0.1:1",
        .catalog_id = "zen-test",
        .catalog_wires = 1,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (!provider)
        return;

    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    struct context context = {.items = items, .n_items = 1, .image_input = 1};
    struct error_log log = {0};
    EXPECT(provider->stream(provider, &context, "gemini-hint", log_error, &log, NULL, NULL) == -1);
    EXPECT(log.n_errors == 1);
    EXPECT(strstr(log.message, "protocol") != NULL);
    provider->destroy(provider);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_list_efforts_wiring();
    test_messages_efforts_follow_thinking_mode();
    test_api_override_stays_in_family();
    test_metadata_api_override();
    test_model_wire_routing();
    test_messages_defaults_follow_preset();
    test_interleaved_reasoning_replay();
    test_unsupported_protocol_reported();
    T_REPORT();
}
