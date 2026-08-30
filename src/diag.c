/* SPDX-License-Identifier: MIT */
#include "diag.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Lets stdout presentation detect diagnostics emitted directly by lower layers. */
static _Atomic unsigned long diagnostic_sequence;

unsigned long hax_diag_sequence(void)
{
    return atomic_load_explicit(&diagnostic_sequence, memory_order_relaxed);
}

static void emit_diagnostic(const char *color, const char *format, va_list args)
{
    int styled = isatty(fileno(stderr)) && *color;
    if (styled)
        fputs(color, stderr);
    fputs("hax: ", stderr);
    vfprintf(stderr, format, args);
    if (styled)
        fputs(ANSI_RESET, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    atomic_fetch_add_explicit(&diagnostic_sequence, 1, memory_order_relaxed);
}

void hax_err(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_diagnostic(theme_open(THEME_ERROR), format, args);
    va_end(args);
}

void hax_warn(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_diagnostic(theme_open(THEME_WARN), format, args);
    va_end(args);
}
