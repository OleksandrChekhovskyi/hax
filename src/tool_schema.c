/* SPDX-License-Identifier: MIT */
#include "tool_schema.h"

#include <jansson.h>
#include <stddef.h>

#include "provider.h"

json_t *tool_schema_build(const struct tool_def *def)
{
    json_t *properties = json_object();
    json_t *required = json_array();

    for (size_t i = 0; def && i < def->n_params; i++) {
        const struct tool_param *param = &def->params[i];
        json_t *prop = json_object();

        if (param->type)
            json_object_set_new(prop, "type", json_string(param->type));
        if (param->item_type)
            json_object_set_new(prop, "items", json_pack("{s:s}", "type", param->item_type));
        if (param->description)
            json_object_set_new(prop, "description", json_string(param->description));
        if (param->minimum)
            json_object_set_new(prop, "minimum", json_integer(param->minimum));
        json_object_set_new(properties, param->name, prop);

        if (param->required)
            json_array_append_new(required, json_string(param->name));
    }

    json_t *schema = json_pack("{s:s, s:o}", "type", "object", "properties", properties);
    if (json_array_size(required) > 0)
        json_object_set_new(schema, "required", required);
    else
        json_decref(required);
    return schema;
}
