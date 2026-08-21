/* SPDX-License-Identifier: MIT */
#include "providers/openai_common.h"

#include <stddef.h>

const char *const OPENAI_EFFORT_LADDER[] = {"none", "minimal", "low", "medium",
                                            "high", "xhigh",   "max"};
const size_t OPENAI_EFFORT_LADDER_N =
    sizeof(OPENAI_EFFORT_LADDER) / sizeof(OPENAI_EFFORT_LADDER[0]);
