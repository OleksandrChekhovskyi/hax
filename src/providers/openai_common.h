/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_COMMON_H
#define HAX_PROVIDERS_OPENAI_COMMON_H

#include <stddef.h>

/* Vocabulary shared by the OpenAI protocol family (the chat and responses wires). */

/* Accepted effort values, ordered from cheapest to most expensive. */
extern const char *const OPENAI_EFFORT_LADDER[];
extern const size_t OPENAI_EFFORT_LADDER_N;

#endif /* HAX_PROVIDERS_OPENAI_COMMON_H */
