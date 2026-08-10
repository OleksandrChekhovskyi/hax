/* SPDX-License-Identifier: MIT */
#include "terminal/ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "text/width.h"

static void ui_line(const char *color, const char *fmt, va_list ap)
{
    /* An empty color (theme off / NO_COLOR) suppresses the closing reset too. */
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

static void pad_spaces(int n)
{
    for (int i = 0; i < n; i++)
        fputc(' ', stdout);
}

void ui_wrapped_rows(const char *text, int indent, int columns, const char *style_open)
{
    int budget = columns - indent;
    if (budget < 1)
        budget = 1;
    int first = 1;
    while (*text || first) {
        size_t separator_bytes = 0;
        size_t row_bytes = *text ? wrap_row_bytes(text, (size_t)budget, &separator_bytes) : 0;
        if (!first)
            pad_spaces(indent);
        if (*style_open)
            fputs(style_open, stdout);
        fwrite(text, 1, row_bytes, stdout);
        if (*style_open)
            fputs(ANSI_RESET, stdout);
        fputc('\n', stdout);
        text += row_bytes + separator_bytes;
        first = 0;
    }
}

void ui_label_row(const char *label, const char *label_open, const char *text,
                  const char *text_open, int text_column, int columns)
{
    fputs("  ", stdout);
    if (*label_open)
        fputs(label_open, stdout);
    fputs(label, stdout);
    if (*label_open)
        fputs(ANSI_RESET, stdout);
    if (columns - text_column >= UI_ROW_MIN_TEXT_CELLS) {
        pad_spaces(text_column - 2 - (int)strlen(label));
        ui_wrapped_rows(text, text_column, columns, text_open);
    } else {
        fputc('\n', stdout);
        pad_spaces(UI_ROW_STACKED_INDENT);
        ui_wrapped_rows(text, UI_ROW_STACKED_INDENT, columns, text_open);
    }
}
