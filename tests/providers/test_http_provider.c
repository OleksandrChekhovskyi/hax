/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "config.h"
#include "harness.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic_messages.h"
#include "providers/http_provider.h"
#include "providers/openai_messages.h"
#include "providers/wire.h"

static void test_list_efforts_wiring(void)
{
    struct http_provider_preset with_efforts = {
        .display_name = "with-efforts",
        .default_base_url = "http://example.invalid/v1",
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
    };
    struct provider *provider = http_provider_new_preset(&with_efforts);
    EXPECT(provider != NULL);
    if (provider) {
        EXPECT(provider->list_models != NULL);
        EXPECT(provider->list_efforts != NULL);
        const char *const *efforts = NULL;
        size_t n_efforts = provider->list_efforts(provider, &efforts);
        EXPECT(n_efforts == OPENAI_EFFORT_LADDER_N);
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

/* On the Messages wire the effort ladder steers adaptive thinking only. */
static void test_messages_efforts_follow_thinking_mode(void)
{
    struct http_provider_preset preset = {
        .display_name = "x",
        .default_base_url = "http://example.invalid/v1",
        .config_prefix = "providers.x",
        .wire = &WIRE_ANTHROPIC_MESSAGES,
        .default_thinking_mode = ANTHROPIC_THINKING_BUDGET,
        .efforts = ANTHROPIC_EFFORT_LADDER,
        .n_efforts = ANTHROPIC_EFFORT_LADDER_N,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    EXPECT(provider != NULL);
    if (provider) {
        const char *const *efforts = NULL;
        EXPECT(provider->list_efforts(provider, &efforts) == 0);

        EXPECT(config_load("{\"providers\": {\"x\": {\"thinking_mode\": \"adaptive\"}}}") == 0);
        EXPECT(provider->list_efforts(provider, &efforts) == ANTHROPIC_EFFORT_LADDER_N);
        EXPECT(efforts != NULL && strcmp(efforts[0], "low") == 0);
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

int main(void)
{
    test_list_efforts_wiring();
    test_messages_efforts_follow_thinking_mode();
    test_api_override_stays_in_family();
    T_REPORT();
}
