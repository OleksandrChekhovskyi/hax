/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_ASK_H
#define HAX_TOOLS_ASK_H

#include "terminal/question.h"

/* Format one model-facing answer line: "Q1: user selected: 1. Foo (value: bar)"
 * or "Q2: user wrote: some text". `label` is the question's display label.
 * Returns allocated text. */
char *ask_format_answer_line(const char *label, const struct ask_answer *answer);

#endif /* HAX_TOOLS_ASK_H */
