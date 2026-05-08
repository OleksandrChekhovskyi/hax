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
 *    so e.g. a heading's bold survives an inline-code cyan span.
 *
 * Note: SGR 22 is "normal intensity" — it closes BOTH bold and dim, so
 * ANSI_BOLD_OFF is the right closer for either. */

#define ANSI_RESET "\x1b[0m"

#define ANSI_BOLD       "\x1b[1m"
#define ANSI_DIM        "\x1b[2m"
#define ANSI_ITALIC     "\x1b[3m"
#define ANSI_BOLD_OFF   "\x1b[22m" /* also closes dim — same SGR */
#define ANSI_ITALIC_OFF "\x1b[23m"

#define ANSI_RED            "\x1b[31m"
#define ANSI_GREEN          "\x1b[32m"
#define ANSI_MAGENTA        "\x1b[35m"
#define ANSI_CYAN           "\x1b[36m"
#define ANSI_BRIGHT_MAGENTA "\x1b[95m"
#define ANSI_FG_DEFAULT     "\x1b[39m"

#define ANSI_ERASE_LINE "\x1b[K"

#endif /* HAX_ANSI_H */
