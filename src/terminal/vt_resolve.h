/* SPDX-License-Identifier: MIT */
#ifndef HAX_VT_RESOLVE_H
#define HAX_VT_RESOLVE_H

#include <stddef.h>
#include <stdio.h>

/* Resolve cursor-addressed render output into plain settled rows.
 *
 * The reader half of `ansi.h`: that header is the escape vocabulary we
 * emit, this one interprets it the way a terminal would. (input_core.c
 * parses the same grammar in the other direction, for keys.)
 *
 * The live display pipeline paints with the cursor: the markdown wrapper
 * retro-wraps a mid-word overflow by walking back and erasing (CSI nD +
 * CSI K, see markdown_wrap.c), the tool preview closes a block by
 * overprinting its leading glyph (\r + glyph, see disp_tool_strip_close),
 * and the user-message echo ends every row with erase-line + \r\n. Those
 * bytes are correct on a terminal and garbage in a file or a pager, which
 * has no cursor to move.
 *
 * vt_resolve replays that byte stream against a one-row cell model and
 * writes each row out only once it is settled — the same content a
 * terminal would be showing, with no cursor motion left in it. That lets
 * the whole live pipeline render into a pipe (the paged history view)
 * without any renderer needing a second, cursor-free code path.
 *
 * Handled: \n (commit row), \r (column 0), CSI nD (back n columns),
 * CSI nC (forward, space-padded), CSI K (erase to end of row, or the
 * whole row for parameters 1/2). SGR runs and any other escape pass
 * through as zero-width bytes, keeping color intact for `less -R`.
 *
 * Cells, not bytes: a column addressed inside a double-width glyph owns
 * that whole glyph (a terminal can't show half of one), and a combining
 * mark rides its base glyph so an erase can't leave an orphan accent
 * behind. Zero-width *escapes*, by contrast, survive an erase — they are
 * terminal state, not content, and the markdown wrapper's retro-wrap
 * depends on that (it re-emits only the style deltas it thinks are
 * missing after erasing).
 *
 * Deliberately NOT modeled: vertical motion (CSI A/B, cursor save /
 * restore) and screen clears — nothing in a history render emits them
 * (the spinner, which does, is disabled for these renders). A stray one
 * passes through inert rather than corrupting the row model.
 *
 * Pure formatting: no isatty, no popen, caller owns the sink. */
void vt_resolve(const char *bytes, size_t n, FILE *out);

#endif /* HAX_VT_RESOLVE_H */
