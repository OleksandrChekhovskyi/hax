/* SPDX-License-Identifier: MIT */
#include "disp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

void disp_emit_held(struct disp *d)
{
    if (d->held == 0)
        return;
    for (int i = 0; i < d->held; i++)
        fputc('\n', stdout);
    d->trail += d->held;
    d->held = 0;
    d->at_space_or_bol = 1; /* cursor is now at column 0 of a fresh line */
}

void disp_putc(struct disp *d, char c)
{
    if (c == '\n') {
        d->held++;
    } else {
        disp_emit_held(d);
        fputc(c, stdout);
        d->trail = 0;
        d->at_space_or_bol = (c == ' ');
    }
}

void disp_write(struct disp *d, const char *s, size_t n)
{
    if (n == 0)
        return;
    /* Walk back across trailing line-ending bytes — both \n and \r — so
     * a CRLF tail (common in Windows files / tool output) is fully
     * deferred and block_separator can collapse it. Only \n counts as a
     * line break for held; \r alone is just a column-zero return. */
    size_t tail_bytes = 0;
    int tail_breaks = 0;
    while (tail_bytes < n) {
        char c = s[n - 1 - tail_bytes];
        if (c == '\n')
            tail_breaks++;
        else if (c != '\r')
            break;
        tail_bytes++;
    }
    if (n > tail_bytes) {
        disp_emit_held(d);
        fwrite(s, 1, n - tail_bytes, stdout);
        d->trail = 0;
        /* Last visible byte committed determines the boundary state.
         * UTF-8 continuation bytes (0x80-0xBF) never equal space, so
         * a multibyte codepoint ending here correctly reads as not-
         * space without any decoder. \r mid-string returns the cursor
         * to col 0 (terminal behavior) — treat it as a fresh-line
         * boundary too. */
        char last = s[n - tail_bytes - 1];
        d->at_space_or_bol = (last == ' ' || last == '\r');
    }
    d->held += tail_breaks;
}

void disp_raw(const char *s)
{
    fputs(s, stdout);
}

void disp_printf(struct disp *d, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    disp_write(d, buf, (size_t)n);
    free(buf);
}

void disp_block_separator(struct disp *d)
{
    int need = 2 - d->trail;
    for (int i = 0; i < need; i++)
        fputc('\n', stdout);
    if (need > 0)
        d->trail += need;
    d->held = 0;
    d->at_space_or_bol = 1; /* cursor at column 0 of separator-fresh row */
}

void disp_first_delta_strip(const struct disp *d, const char **s, size_t *n)
{
    if (d->saw_text)
        return;
    while (*n > 0 && (**s == '\n' || **s == '\r')) {
        (*s)++;
        (*n)--;
    }
}
