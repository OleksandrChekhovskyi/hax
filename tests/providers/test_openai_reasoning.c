/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <string.h>

#include "harness.h"
#include "providers/openai.h"

/* Encode into a fresh object; caller decrefs. */
static json_t *encode(enum reasoning_format fmt, const char *effort)
{
    json_t *body = json_object();
    openai_apply_reasoning(body, fmt, effort);
    return body;
}

static void test_unset_omits(void)
{
    /* No effort — and empty, which counts as unset — leaves the field off
     * entirely for both dialects, so the provider's own default stands. */
    const char *empties[] = {NULL, ""};
    for (size_t i = 0; i < 2; i++) {
        json_t *flat = encode(REASONING_FLAT, empties[i]);
        json_t *nested = encode(REASONING_NESTED, empties[i]);
        EXPECT(json_object_size(flat) == 0);
        EXPECT(json_object_size(nested) == 0);
        json_decref(flat);
        json_decref(nested);
    }
}

static void test_flat(void)
{
    /* Flat form is a single scalar; "none" passes straight through for the
     * server to interpret (no client-side gate). */
    json_t *b = encode(REASONING_FLAT, "high");
    EXPECT(json_object_get(b, "reasoning") == NULL);
    EXPECT_STR_EQ(json_string_value(json_object_get(b, "reasoning_effort")), "high");
    json_decref(b);

    b = encode(REASONING_FLAT, "none");
    EXPECT_STR_EQ(json_string_value(json_object_get(b, "reasoning_effort")), "none");
    json_decref(b);
}

static void test_nested_effort(void)
{
    /* A real effort enables and passes the level through. */
    json_t *b = encode(REASONING_NESTED, "high");
    EXPECT(json_object_get(b, "reasoning_effort") == NULL);
    json_t *r = json_object_get(b, "reasoning");
    EXPECT(json_is_object(r));
    EXPECT(json_is_true(json_object_get(r, "enabled")));
    EXPECT_STR_EQ(json_string_value(json_object_get(r, "effort")), "high");
    json_decref(b);
}

static void test_nested_none_disables(void)
{
    /* "none" disables via enabled:false and omits effort entirely — the
     * canonical off switch, not an effort value the router might reject. */
    json_t *b = encode(REASONING_NESTED, "none");
    json_t *r = json_object_get(b, "reasoning");
    EXPECT(json_is_object(r));
    EXPECT(json_is_false(json_object_get(r, "enabled")));
    EXPECT(json_object_get(r, "effort") == NULL);
    json_decref(b);
}

int main(void)
{
    test_unset_omits();
    test_flat();
    test_nested_effort();
    test_nested_none_disables();
    T_REPORT();
}
