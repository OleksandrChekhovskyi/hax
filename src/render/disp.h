/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_DISP_H
#define HAX_RENDER_DISP_H

#include <stddef.h>
#include <stdio.h>

/* Block-aware output for separating visual blocks with exactly one blank line.
 *
 * Content writes defer trailing line endings so disp_block_separator() can collapse them to the
 * two newlines between blocks. A later visible byte instead commits pending newlines verbatim.
 *
 * The sink is borrowed. Direct visible writes must resynchronize newline state before more content
 * passes through the display; disp_sync_external_line() records the common one-line case. */
struct disp {
    FILE *sink; /* borrowed; NULL selects stdout */
    size_t committed_newlines;
    size_t pending_newlines;
};

FILE *disp_sink(const struct disp *disp);
void disp_flush(struct disp *disp);

/* Synchronize after output written outside disp ends with one newline. */
void disp_sync_external_line(struct disp *disp);

/* Commit all pending newlines without collapsing them. */
void disp_commit_newlines(struct disp *disp);

/* Newlines remain pending until visible content, disp_commit_newlines(), or a block separator. */
void disp_putc(struct disp *disp, char byte);

/* Trailing LF and CRLF endings become pending; bare carriage returns preserve newline state. */
void disp_write(struct disp *disp, const char *bytes, size_t len);

/* Write zero-width terminal control bytes without changing newline state. */
void disp_write_ansi(struct disp *disp, const char *bytes);

__attribute__((format(printf, 2, 3), nonnull(2))) void disp_printf(struct disp *disp,
                                                                   const char *format, ...);

/* End the previous block with exactly one blank line, collapsing pending line endings. */
void disp_block_separator(struct disp *disp);

#endif /* HAX_RENDER_DISP_H */
