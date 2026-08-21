/* SPDX-License-Identifier: MIT */
#ifndef HAX_RENDER_CTRL_STRIP_H
#define HAX_RENDER_CTRL_STRIP_H

#include <stddef.h>

/* Stateful sanitizer for untrusted terminal output. It removes 7-bit ECMA-48 escape sequences and
 * C0 controls other than HT and LF. DEL is also removed; bytes at or above 0x80 are preserved to
 * avoid corrupting UTF-8. LF, CAN, and SUB cancel an incomplete escape sequence.
 *
 * State carries across feed calls, and output is never longer than input. */
enum ctrl_strip_state {
    CTRL_STRIP_TEXT,
    CTRL_STRIP_ESCAPE,
    CTRL_STRIP_CSI,
    CTRL_STRIP_OSC,
    CTRL_STRIP_OSC_ESCAPE,
    CTRL_STRIP_CONTROL_STRING,
    CTRL_STRIP_CONTROL_STRING_ESCAPE,
    CTRL_STRIP_ESCAPE_INTERMEDIATE,
};

struct ctrl_strip {
    enum ctrl_strip_state state;
};

void ctrl_strip_init(struct ctrl_strip *strip);

/* Write sanitized input to a non-overlapping output buffer. Returns the number of bytes written. */
size_t ctrl_strip_feed(struct ctrl_strip *strip, const char *input, size_t input_len, char *output);

/* Return a newly allocated, sanitized copy of a NUL-terminated string. Caller frees it. */
char *ctrl_strip_dup(const char *input);

/* ctrl_strip_dup for single-line contexts: HT and LF become spaces, so an untrusted value
 * cannot break the line it is embedded in. Caller frees the copy. */
char *ctrl_strip_line_dup(const char *input);

#endif /* HAX_RENDER_CTRL_STRIP_H */
