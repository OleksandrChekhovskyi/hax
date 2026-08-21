/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>

#include "harness.h"
#include "providers/opencode.h"
#include "providers/usage_render.h"

#define WINDOWS_MAX 8

static size_t parse(const char *body, struct usage_window *windows, size_t max, json_t **root)
{
    *root = json_loads(body, 0, NULL);
    EXPECT(*root != NULL);
    if (!*root)
        return 0;
    return opencode_usage_parse(*root, windows, max);
}

static void test_live_response_shape(void)
{
    struct usage_window windows[WINDOWS_MAX];
    json_t *root;
    size_t n = parse("{\"usage\":{"
                     "\"rolling\":{\"status\":\"ok\",\"percent\":5,"
                     "\"resetsAt\":\"2026-08-21T21:14:51.969Z\"},"
                     "\"weekly\":{\"status\":\"ok\",\"percent\":54,"
                     "\"resetsAt\":\"2026-08-24T00:00:00.969Z\"},"
                     "\"monthly\":{\"status\":\"ok\",\"percent\":27,"
                     "\"resetsAt\":\"2026-09-19T13:32:19.969Z\"}}}",
                     windows, WINDOWS_MAX, &root);

    EXPECT(n == 3);
    EXPECT_STR_EQ(windows[0].label, "rolling");
    EXPECT(windows[0].used_percent == 5);
    EXPECT(windows[0].reset_at == 1787346891);
    EXPECT(windows[0].note == NULL);
    EXPECT_STR_EQ(windows[1].label, "weekly");
    EXPECT(windows[1].used_percent == 54);
    EXPECT(windows[1].reset_at == 1787529600);
    EXPECT_STR_EQ(windows[2].label, "monthly");
    EXPECT(windows[2].reset_at == 1789824739);
    json_decref(root);
}

static void test_non_ok_status_becomes_note(void)
{
    struct usage_window windows[WINDOWS_MAX];
    json_t *root;
    size_t n = parse("{\"usage\":{\"weekly\":{\"status\":\"limited\",\"percent\":100,"
                     "\"resetsAt\":\"2026-08-24T00:00:00Z\"}}}",
                     windows, WINDOWS_MAX, &root);

    EXPECT(n == 1);
    EXPECT_STR_EQ(windows[0].note, "limited");
    json_decref(root);
}

static void test_malformed_windows_skipped(void)
{
    struct usage_window windows[WINDOWS_MAX];
    json_t *root;
    size_t n = parse("{\"usage\":{"
                     "\"no_percent\":{\"resetsAt\":\"2026-08-24T00:00:00Z\"},"
                     "\"no_reset\":{\"percent\":5},"
                     "\"reset_not_utc\":{\"percent\":5,\"resetsAt\":\"2026-08-24T00:00:00+02:00\"},"
                     "\"reset_garbage\":{\"percent\":5,\"resetsAt\":\"tomorrow\"},"
                     "\"day_overflow\":{\"percent\":5,\"resetsAt\":\"2026-02-31T00:00:00Z\"},"
                     "\"nonleap_feb29\":{\"percent\":5,\"resetsAt\":\"2100-02-29T00:00:00Z\"},"
                     "\"empty_fraction\":{\"percent\":5,\"resetsAt\":\"2026-08-24T00:00:00.Z\"},"
                     "\"not_object\":42,"
                     "\"good\":{\"percent\":5,\"resetsAt\":\"2026-08-24T00:00:00Z\"}}}",
                     windows, WINDOWS_MAX, &root);

    EXPECT(n == 1);
    EXPECT_STR_EQ(windows[0].label, "good");
    json_decref(root);
}

static void test_unexpected_roots_yield_nothing(void)
{
    struct usage_window windows[WINDOWS_MAX];
    json_t *root;

    size_t n = parse("{\"error\":\"nope\"}", windows, WINDOWS_MAX, &root);
    EXPECT(n == 0);
    json_decref(root);

    n = parse("{\"usage\":[]}", windows, WINDOWS_MAX, &root);
    EXPECT(n == 0);
    json_decref(root);
}

static void test_window_count_capped(void)
{
    struct usage_window windows[2];
    json_t *root;
    size_t n = parse("{\"usage\":{"
                     "\"a\":{\"percent\":1,\"resetsAt\":\"2026-08-24T00:00:00Z\"},"
                     "\"b\":{\"percent\":2,\"resetsAt\":\"2026-08-24T00:00:00Z\"},"
                     "\"c\":{\"percent\":3,\"resetsAt\":\"2026-08-24T00:00:00Z\"}}}",
                     windows, 2, &root);

    EXPECT(n == 2);
    EXPECT_STR_EQ(windows[1].label, "b");
    json_decref(root);
}

static void test_timestamp_conversion(void)
{
    struct usage_window windows[WINDOWS_MAX];
    json_t *root;
    size_t n = parse("{\"usage\":{"
                     "\"epoch\":{\"percent\":0,\"resetsAt\":\"1970-01-01T00:00:00Z\"},"
                     "\"leap\":{\"percent\":0,\"resetsAt\":\"2000-02-29T12:00:00Z\"}}}",
                     windows, WINDOWS_MAX, &root);

    EXPECT(n == 2);
    EXPECT(windows[0].reset_at == 0);
    EXPECT(windows[1].reset_at == 951825600);
    json_decref(root);
}

int main(void)
{
    test_live_response_shape();
    test_non_ok_status_becomes_note();
    test_malformed_windows_skipped();
    test_unexpected_roots_yield_nothing();
    test_window_count_capped();
    test_timestamp_conversion();
    T_REPORT();
}
