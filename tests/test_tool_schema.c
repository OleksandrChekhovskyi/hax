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

int main(void)
{
    test_empty_def_yields_object_schema();
    test_primitive_params();
    test_array_param_emits_item_type();
    T_REPORT();
}
