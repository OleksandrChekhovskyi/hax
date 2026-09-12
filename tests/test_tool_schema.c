/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stddef.h>

#include "harness.h"
#include "provider.h"
#include "tool_schema.h"

static void test_empty_def_yields_object_schema(void)
{
    struct tool_def def = {.name = "noop"};
    json_t *schema = tool_schema_build(&def);
    EXPECT_STR_EQ(json_string_value(json_object_get(schema, "type")), "object");
    EXPECT(json_object_get(schema, "properties") != NULL);
    EXPECT(json_object_get(schema, "required") == NULL);
    json_decref(schema);
}

static void test_primitive_params(void)
{
    static const struct tool_param params[] = {
        {.name = "command", .type = "string", .required = 1, .description = "Shell command."},
        {.name = "timeout_seconds", .type = "integer", .minimum = 1},
    };
    struct tool_def def = {.name = "bash", .params = params, .n_params = 2};
    json_t *schema = tool_schema_build(&def);

    json_t *properties = json_object_get(schema, "properties");
    json_t *command = json_object_get(properties, "command");
    EXPECT_STR_EQ(json_string_value(json_object_get(command, "type")), "string");
    EXPECT_STR_EQ(json_string_value(json_object_get(command, "description")), "Shell command.");
    json_t *timeout = json_object_get(properties, "timeout_seconds");
    EXPECT(json_integer_value(json_object_get(timeout, "minimum")) == 1);
    EXPECT(json_object_get(timeout, "items") == NULL);

    json_t *required = json_object_get(schema, "required");
    EXPECT(json_array_size(required) == 1);
    EXPECT_STR_EQ(json_string_value(json_array_get(required, 0)), "command");
    json_decref(schema);
}

static void test_array_param_emits_item_type(void)
{
    static const struct tool_param params[] = {
        {.name = "ids", .type = "array", .item_type = "string", .required = 1},
    };
    struct tool_def def = {.name = "batch", .params = params, .n_params = 1};
    json_t *schema = tool_schema_build(&def);

    json_t *ids = json_object_get(json_object_get(schema, "properties"), "ids");
    EXPECT_STR_EQ(json_string_value(json_object_get(ids, "type")), "array");
    json_t *items = json_object_get(ids, "items");
    EXPECT(json_is_object(items));
    EXPECT_STR_EQ(json_string_value(json_object_get(items, "type")), "string");
    json_decref(schema);
}

static void test_raw_schema_json_used_verbatim(void)
{
    static const char raw[] = "{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":"
                              "{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}";
    static const struct tool_param params[] = {
        {.name = "edits", .schema_json = raw, .required = 1},
        {.name = "note", .type = "string", .description = "Free note."},
    };
    struct tool_def def = {.name = "patch", .params = params, .n_params = 2};
    json_t *schema = tool_schema_build(&def);

    json_t *properties = json_object_get(schema, "properties");
    json_t *edits = json_object_get(properties, "edits");
    EXPECT_STR_EQ(json_string_value(json_object_get(edits, "type")), "array");
    json_t *items = json_object_get(edits, "items");
    EXPECT(json_is_object(items));
    json_t *path = json_object_get(json_object_get(items, "properties"), "path");
    EXPECT_STR_EQ(json_string_value(json_object_get(path, "type")), "string");

    json_t *note = json_object_get(properties, "note");
    EXPECT_STR_EQ(json_string_value(json_object_get(note, "type")), "string");
    EXPECT_STR_EQ(json_string_value(json_object_get(note, "description")), "Free note.");
    json_decref(schema);
}

static void test_raw_schema_json_parse_failure_falls_back(void)
{
    static const struct tool_param params[] = {
        {.name = "command", .type = "string", .schema_json = "{not json"},
    };
    struct tool_def def = {.name = "bash", .params = params, .n_params = 1};
    json_t *schema = tool_schema_build(&def);

    json_t *command = json_object_get(json_object_get(schema, "properties"), "command");
    EXPECT_STR_EQ(json_string_value(json_object_get(command, "type")), "string");
    json_decref(schema);
}

int main(void)
{
    test_empty_def_yields_object_schema();
    test_primitive_params();
    test_array_param_emits_item_type();
    test_raw_schema_json_used_verbatim();
    test_raw_schema_json_parse_failure_falls_back();
    T_REPORT();
}
