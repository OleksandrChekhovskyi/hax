/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_VERTEX_WIRE_H
#define HAX_PROVIDERS_VERTEX_WIRE_H

#include "providers/wire.h"

/* Vertex serves Claude with the Anthropic Messages dialect, but its endpoint names the model in
 * the path and asks for the dialect's version in the body: the body must not carry the model,
 * which the endpoint rejects as an extra field. */
extern const struct wire WIRE_ANTHROPIC_MESSAGES_VERTEX;

#endif /* HAX_PROVIDERS_VERTEX_WIRE_H */
