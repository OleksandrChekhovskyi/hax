/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_VT_RESOLVE_H
#define HAX_TERMINAL_VT_RESOLVE_H

#include <stddef.h>
#include <stdio.h>

/* Replay cursor-addressed bytes against a one-row terminal model and write settled rows to out.
 *
 * Handles LF, CR, CSI nD/nC, and CSI K modes 0-2. Other escape sequences pass through as
 * zero-width terminal state. Cursor movement uses display cells, keeping wide glyphs and combining
 * marks intact. A nonempty final row is terminated with LF.
 *
 * SGR state is tracked across rows and each settled row is emitted style-self-contained: styling
 * carried in from earlier rows reopens before the row's content, and styling still open at the
 * row end is reset. Pagers such as less -R reset attributes at every line, so styling that relies
 * on terminal state persisting across LF would otherwise degrade past its first row.
 *
 * bytes is borrowed for the call; out is borrowed and remains open. */
void vt_resolve(const char *bytes, size_t len, FILE *out);

#endif /* HAX_TERMINAL_VT_RESOLVE_H */
