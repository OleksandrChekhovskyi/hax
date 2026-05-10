/* SPDX-License-Identifier: MIT */
#include "render/disp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"
#include "terminal/ansi.h"

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

/* All three eager strip variants share the same envelope: ANSI_DIM_CYAN
 * sets dim cyan, the box-drawing glyph and trailing space land in that
 * style, then ANSI_RESET clears both attributes so callers can apply
 * their own SGR to the content that follows. */

void disp_tool_strip(struct disp *d)
{
    /* dim+cyan │ (U+2502, 3-byte UTF-8) space reset */
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x94\x82 " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

void disp_tool_strip_first(struct disp *d)
{
    /* dim+cyan ┌ (U+250C, 3-byte UTF-8) space reset */
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x94\x8C " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

void disp_tool_strip_solo(struct disp *d)
{
    /* dim+cyan › (U+203A, 3-byte UTF-8) space reset */
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x80\xBA " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

/* Shared overprint: \r back to col 0 of the current row, redraw the
 * leading glyph in dim cyan, reset SGR. The "┌" or "│" originally
 * there is replaced by `glyph_utf8` (3-byte UTF-8 expected, single
 * cell). The space at col 1 and content from col 2 onward survive
 * untouched. Cursor lands at col 1; the next held-\n flush moves
 * down to a fresh row. */
static void tool_strip_overprint(const char *glyph_utf8)
{
    fputs("\r" ANSI_DIM_CYAN, stdout);
    fputs(glyph_utf8, stdout);
    fputs(ANSI_RESET, stdout);
    fflush(stdout);
}

void disp_tool_strip_close(void)
{
    tool_strip_overprint("\xE2\x94\x94"); /* └ U+2514 */
}

void disp_tool_strip_close_solo(void)
{
    tool_strip_overprint("\xE2\x80\xBA"); /* › U+203A */
}
