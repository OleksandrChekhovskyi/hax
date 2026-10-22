/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "providers/vertex.h"
#include "providers/vertex_wire.h"

/* A Gemini model travels to the OpenAI-compatible endpoint under its publisher's name. */
static void test_rewrites_a_gemini_model(void)
{
    char *rewritten = vertex_rewrite_model("gemini-3.1-pro-preview");
    EXPECT(rewritten != NULL);
    if (rewritten)
        EXPECT_STR_EQ(rewritten, "google/gemini-3.1-pro-preview");
    free(rewritten);
}

/* A model that names its publisher already keeps its name. */
static void test_keeps_a_qualified_model(void)
{
    EXPECT(vertex_rewrite_model("openai/gpt-oss-120b-maas") == NULL);
}

/* A Claude model travels to the Anthropic endpoint, which names it in the path: the wire id
 * would carry the publisher the path does not. */
static void test_keeps_a_claude_model(void)
{
    EXPECT(vertex_rewrite_model("claude-sonnet-4-6@default") == NULL);
}

/* A Claude model speaks the Anthropic dialect; a Gemini model speaks the provider's own. */
static void test_routes_the_wire_by_model(void)
{
    const struct wire *claude = vertex_wire_for_model(NULL, "claude-sonnet-4-6@default");
    EXPECT(claude == &WIRE_ANTHROPIC_MESSAGES_VERTEX);
    EXPECT(vertex_wire_for_model(NULL, "gemini-3.1-pro-preview") == NULL);
}

/* The Claude endpoint names the model in its path, so the request's base carries it. */
static void test_builds_the_claude_request_base(void)
{
    setenv("VERTEXAI_PROJECT", "test-project", 1);
    setenv("VERTEXAI_LOCATION", "us-east5", 1);
    unsetenv("HAX_VERTEX_BASE_URL");

    char *base = vertex_request_base(NULL, "claude-sonnet-4-6@default");
    EXPECT(base != NULL);
    if (base) {
        EXPECT(strstr(base, "/v1/projects/test-project/locations/us-east5/publishers/anthropic/"
                            "models/claude-sonnet-4-6@default") != NULL);
        EXPECT(strstr(base, "https://us-east5-aiplatform.googleapis.com") != NULL);
    }
    free(base);

    /* A Gemini model keeps the provider's own base URL. */
    EXPECT(vertex_request_base(NULL, "gemini-3.1-pro-preview") == NULL);

    unsetenv("VERTEXAI_PROJECT");
    unsetenv("VERTEXAI_LOCATION");
}

/* A model named with its publisher reaches the same path. */
static void test_builds_the_claude_request_base_with_a_publisher(void)
{
    setenv("VERTEXAI_PROJECT", "test-project", 1);
    setenv("VERTEXAI_LOCATION", "us-east5", 1);
    unsetenv("HAX_VERTEX_BASE_URL");

    EXPECT(vertex_model_is_anthropic("anthropic/claude-sonnet-4-6@default"));
    char *base = vertex_request_base(NULL, "anthropic/claude-sonnet-4-6@default");
    EXPECT(base != NULL);
    if (base)
        EXPECT(strstr(base, "publishers/anthropic/models/claude-sonnet-4-6@default") != NULL);
    free(base);

    unsetenv("VERTEXAI_PROJECT");
    unsetenv("VERTEXAI_LOCATION");
}

int main(void)
{
    test_rewrites_a_gemini_model();
    test_keeps_a_qualified_model();
    test_keeps_a_claude_model();
    test_routes_the_wire_by_model();
    test_builds_the_claude_request_base();
    test_builds_the_claude_request_base_with_a_publisher();
    T_REPORT();
}
