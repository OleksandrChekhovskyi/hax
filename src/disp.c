/* SPDX-License-Identifier: MIT */
#include "disp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "ansi.h"
#include "util.h"

void disp_emit_held(struct disp *d)
{
    for (int i = 0; i < d->held; i++)
        fputc('\n', stdout);
    d->trail += d->held;
    d->held = 0;
}

void disp_putc(struct disp *d, char c)
{
    if (c == '\n') {
        d->held++;
    } else {
        disp_emit_held(d);
        fputc(c, stdout);
        d->trail = 0;
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

/* All four strip variants share the same envelope: ANSI_DIM_CYAN sets
 * dim cyan (so the gutter glyphs read as a quiet cyan rule), the
 * box-drawing glyph and trailing space land in that style, then
 * ANSI_RESET clears both attributes so callers can apply their own
 * SGR to the content that follows. */

void disp_tool_strip(struct disp *d)
{
    /* dim+cyan  │(U+2502, 3-byte UTF-8)  space  reset */
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x94\x82 " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

void disp_tool_strip_first(struct disp *d)
{
    /* dim+cyan  ┌(U+250C, 3-byte UTF-8)  space  reset */
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x94\x8C " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

void disp_tool_strip_last(struct disp *d)
{
    /* dim+cyan  └(U+2514, 3-byte UTF-8)  space  reset */
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x94\x94 " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

void disp_tool_strip_solo(struct disp *d)
{
    /* dim+cyan  ›(U+203A, 3-byte UTF-8)  space  reset */
    static const char strip[] = ANSI_DIM_CYAN "\xE2\x80\xBA " ANSI_RESET;
    disp_write(d, strip, sizeof(strip) - 1);
}

/* Shared overprint implementation for the diff path: \r back to col 0
 * of the current row, redraw the leading glyph in dim cyan, reset SGR.
 * The top-corner "┌" originally there (or "│" body row) is replaced by
 * `glyph` (3-byte UTF-8 expected, single cell). The space at col 1 and
 * content from col 2 onward survive untouched. Cursor lands at col 1;
 * the next held-\n flush moves it down to a fresh row. */
static void tool_strip_overprint(const char *glyph_utf8)
{
    fputs("\r" ANSI_DIM_CYAN, stdout);
    fputs(glyph_utf8, stdout);
    fputs(ANSI_RESET, stdout);
    fflush(stdout);
}

void disp_tool_strip_close(void)
{
    tool_strip_overprint("\xE2\x94\x94"); /* └  U+2514 */
}

void disp_tool_strip_close_solo(void)
{
    tool_strip_overprint("\xE2\x80\xBA"); /* ›  U+203A */
}
