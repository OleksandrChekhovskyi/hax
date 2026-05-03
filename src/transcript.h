/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSCRIPT_H
#define HAX_TRANSCRIPT_H

#include <stddef.h>
#include <stdio.h>

struct item;

/* Render the conversation as the model sees it — system prompt, then each
 * item in order, all unshortened. ANSI styling is included so the output
 * pages cleanly through `less -R`. Pure formatting: no popen, no isatty
 * checks, no errno handling — the caller decides where bytes go. */
void transcript_render(FILE *out, const char *system_prompt, const struct item *items,
                       size_t n_items);

#endif /* HAX_TRANSCRIPT_H */
