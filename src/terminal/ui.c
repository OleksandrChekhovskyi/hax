/* SPDX-License-Identifier: MIT */
#include "terminal/ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Emit one colored status line on stdout. Color is gated on stdout being a
 * TTY so a piped/redirected REPL doesn't collect raw escape bytes — matches
 * the rest of the display layer. The trailing newline is always written. */
static void ui_line(const char *color, const char *fmt, va_list ap)
{
    /* An empty color (theme off / NO_COLOR) suppresses the closing reset
     * too — no escapes at all on a colorless line. */
    int styled = isatty(fileno(stdout)) && *color;
    if (styled)
        fputs(color, stdout);
    vprintf(fmt, ap);
    if (styled)
        fputs(ANSI_RESET, stdout);
    putchar('\n');
}

void ui_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(theme_open(THEME_ERROR), fmt, ap);
    va_end(ap);
}

void ui_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(ANSI_DIM, fmt, ap);
    va_end(ap);
}
