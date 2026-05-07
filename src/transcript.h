/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSCRIPT_H
#define HAX_TRANSCRIPT_H

#include <stddef.h>
#include <stdio.h>

struct item;
struct tool_def;

/* Render the conversation as the model sees it — system prompt, advertised
 * tools, then each item in order, all unshortened. ANSI styling is
 * included so the output pages cleanly through `less -R`. Pure
 * formatting: no popen, no isatty checks, no errno handling — the caller
 * decides where bytes go. Pass tools=NULL / n_tools=0 to omit the tools
 * section (matches --raw mode, where no tools are advertised). */
void transcript_render(FILE *out, const char *system_prompt, const struct tool_def *tools,
                       size_t n_tools, const struct item *items, size_t n_items);

#endif /* HAX_TRANSCRIPT_H */
