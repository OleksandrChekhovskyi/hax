/* SPDX-License-Identifier: MIT */
#ifndef HAX_ANSI_H
#define HAX_ANSI_H

/* ANSI/SGR escape sequences as string literals, so adjacent-string
 * concatenation works at compile time (e.g. `"\r" ANSI_ERASE_LINE`).
 *
 * Two flavors of closer:
 *  - ANSI_RESET (`0m`) clears every attribute — use when we don't care
 *    what was open.
 *  - Per-attribute closers (`22m`, `23m`, `39m`) leave the others intact,
 *    so e.g. a heading's bold survives an inline-code color span.
 *
 * Note: SGR 22 is "normal intensity" — it closes BOTH bold and dim, so
 * ANSI_BOLD_OFF is the right closer for either. */

#define ANSI_RESET "\x1b[0m"

#define ANSI_BOLD       "\x1b[1m"
#define ANSI_DIM        "\x1b[2m"
#define ANSI_ITALIC     "\x1b[3m"
#define ANSI_BOLD_OFF   "\x1b[22m" /* also closes dim — same SGR */
#define ANSI_ITALIC_OFF "\x1b[23m"

/* The 16-color palette entries below are the raw vocabulary for
 * terminal/theme.c's "ansi" preset (and for tests). Production code
 * should color through the semantic roles in terminal/theme.h; direct
 * use here is for attributes (bold/dim/italic) and control sequences. */
#define ANSI_RED            "\x1b[31m"
#define ANSI_GREEN          "\x1b[32m"
#define ANSI_YELLOW         "\x1b[33m"
#define ANSI_CYAN           "\x1b[36m"
#define ANSI_BRIGHT_MAGENTA "\x1b[95m"
#define ANSI_FG_DEFAULT     "\x1b[39m"

#define ANSI_ERASE_LINE "\x1b[K"

/* Erase from the cursor to the end of the screen (ED 0). */
#define ANSI_ERASE_BELOW "\x1b[J"

/* DEC private mode 2026 — synchronized output. Between BEGIN and END the
 * terminal buffers everything we write and presents it as one atomic
 * frame, so a multi-row prompt repaint can't show an intermediate blank
 * (erased-but-not-yet-redrawn) state. Terminals that don't implement the
 * mode ignore the unknown private mode harmlessly. The prompt editor
 * wraps each full-area repaint in this pair; interrupt.c's tty restore
 * also emits END so a paint interrupted mid-frame can't leave the
 * terminal stuck with updates suspended. */
#define ANSI_SYNC_BEGIN "\x1b[?2026h"
#define ANSI_SYNC_END   "\x1b[?2026l"

/* DECTCEM cursor visibility. Hidden during model streaming and tool
 * dispatch so the only "we're alive" indicator is the spinner glyph;
 * shown only for the duration of the input prompt. Restoration on
 * exit / signal lives in interrupt.c's restore_tty_only(). */
#define ANSI_CURSOR_HIDE "\x1b[?25l"
#define ANSI_CURSOR_SHOW "\x1b[?25h"

#endif /* HAX_ANSI_H */
