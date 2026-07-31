/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_SCHEMA_H
#define HAX_TOOL_SCHEMA_H

#include <jansson.h>

struct tool_def;

/* Build the JSON Schema object describing a tool's parameters. Returns a new reference the caller
 * owns; never NULL. A tool with no parameters yields a property-less object schema. */
json_t *tool_schema_build(const struct tool_def *def);

#endif /* HAX_TOOL_SCHEMA_H */
