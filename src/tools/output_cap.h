/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_OUTPUT_CAP_H
#define HAX_TOOLS_OUTPUT_CAP_H

#include <stddef.h>

/* Tool-result limits applied before output is sent to the model. */
size_t output_cap_bytes(void);
#define OUTPUT_CAP_LINES      2000
#define OUTPUT_CAP_LINE_WIDTH 500

/* Replace each line suffix beyond max_line_bytes with an elision marker while preserving newline
 * structure. Returns a newly allocated NUL-terminated buffer and stores its length in out_len. */
char *cap_line_lengths(const char *data, size_t length, size_t max_line_bytes, size_t *out_len);

/* Visible separator between read-tool line numbers and content. Unlike whitespace, it cannot be
 * mistaken for indentation when a model copies text into an edit request. */
#define READ_LINE_DELIM "\xE2\x86\x92"

#endif /* HAX_TOOLS_OUTPUT_CAP_H */
