/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <string.h>

#include "harness.h"
#include "text/json_scan.h"

static json_t *find(const char *text, const char *key)
{
    struct json_scan_entry member;
    return json_scan_find(text, key, &member) == 1 ? json_scan_load(&member) : NULL;
}

/* Keys match exactly, later members are reachable past earlier ones, and escaped quotes and
 * brackets inside strings don't derail the scan. */
static void test_find_member(void)
{
    const char *text = "{\n"
                       "  \"open\": {\"models\": {}},\n"
                       "  \"tricky\": {\"s\": \"esc \\\" } ] {\", \"a\": [1, {\"b\": []}]},\n"
                       "  \"openai\": {\"models\": {\"m\": {\"cost\": {\"input\": 2}}}, \"n\": 1}\n"
                       "}";
    json_t *value = find(text, "openai");
    EXPECT(json_is_object(json_object_get(value, "models")));
    json_decref(value);

    value = find(text, "tricky");
    EXPECT_STR_EQ(json_string_value(json_object_get(value, "s")), "esc \" } ] {");
    json_decref(value);

    value = find("{\"n\": 42}", "n");
    EXPECT(json_is_integer(value) && json_integer_value(value) == 42);
    json_decref(value);

    struct json_scan_entry member;
    EXPECT(json_scan_find(text, "ope", &member) == 0);
    EXPECT(json_scan_find(text, "openai2", &member) == 0);
    EXPECT(json_scan_find(text, "models", &member) == 0); /* nested, not top-level */
    EXPECT(json_scan_find("{}", "k", &member) == 0);
    EXPECT(json_scan_find("[1, 2]", "k", &member) == -1);
    EXPECT(json_scan_find("null", "k", &member) == -1);
    EXPECT(json_scan_find("{", "k", &member) == -1);
    EXPECT(json_scan_find("{\"k\": {\"a\": 1}", "k", &member) == -1); /* unterminated root */
    EXPECT(json_scan_find("{\"k\": \"unterminated", "k", &member) == -1);
}

static void test_array_elements(void)
{
    struct json_scan scan;
    struct json_scan_entry element;
    const char *text = " [ {\"id\": \"a]\"}, 7 , \"x\", [[]] ] tail";

    EXPECT(json_scan_array(&scan, text) == 0);
    const char *expected[] = {"{\"id\": \"a]\"}", "7", "\"x\"", "[[]]"};
    for (size_t i = 0; i < 4; i++) {
        EXPECT(json_scan_next(&scan, &element) == 1);
        EXPECT(element.key == NULL);
        EXPECT(element.value_len == strlen(expected[i]) &&
               memcmp(element.value, expected[i], element.value_len) == 0);
    }
    EXPECT(json_scan_next(&scan, &element) == 0);
    EXPECT_STR_EQ(scan.cursor, " tail");

    EXPECT(json_scan_array(&scan, "[ ]") == 0);
    EXPECT(json_scan_next(&scan, &element) == 0);

    EXPECT(json_scan_array(&scan, "{}") == -1);
    EXPECT(json_scan_object(&scan, "[]") == -1);

    EXPECT(json_scan_array(&scan, "[1,]") == 0);
    EXPECT(json_scan_next(&scan, &element) == 1);
    EXPECT(json_scan_next(&scan, &element) == -1);

    EXPECT(json_scan_array(&scan, "[1 2]") == 0);
    EXPECT(json_scan_next(&scan, &element) == -1);

    EXPECT(json_scan_array(&scan, "[{\"a\": 1}") == 0);
    EXPECT(json_scan_next(&scan, &element) == -1);
}

/* The scan follows structure only; loading is what validates a value. */
static void test_load_validates_value(void)
{
    struct json_scan scan;
    struct json_scan_entry element;
    EXPECT(json_scan_array(&scan, "[{\"a\" 1}, tru]") == 0);
    EXPECT(json_scan_next(&scan, &element) == 1);
    EXPECT(json_scan_load(&element) == NULL);
    EXPECT(json_scan_next(&scan, &element) == 1);
    EXPECT(json_scan_load(&element) == NULL);
}

int main(void)
{
    test_find_member();
    test_array_elements();
    test_load_validates_value();
    T_REPORT();
}
