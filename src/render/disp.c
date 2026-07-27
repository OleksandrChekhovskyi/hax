/* SPDX-License-Identifier: MIT */
#include "render/disp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

FILE *disp_sink(const struct disp *d)
{
    return d->out ? d->out : stdout;
}

void disp_flush(struct disp *d)
{
    fflush(disp_sink(d));
}

void disp_emit_held(struct disp *d)
{
    if (d->held == 0)
        return;
    FILE *out = disp_sink(d);
    for (int i = 0; i < d->held; i++)
        fputc('\n', out);
    d->trail += d->held;
    d->held = 0;
}

void disp_putc(struct disp *d, char c)
{
    if (c == '\n') {
        d->held++;
    } else {
        disp_emit_held(d);
        fputc(c, disp_sink(d));
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
        fwrite(s, 1, n - tail_bytes, disp_sink(d));
        d->trail = 0;
    }
    d->held += tail_breaks;
}

void disp_raw(struct disp *d, const char *s)
{
    fputs(s, disp_sink(d));
}

void disp_printf(struct disp *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *buf = xvasprintf(fmt, ap);
    va_end(ap);
    if (!buf)
        return;
    disp_write(d, buf, strlen(buf));
    free(buf);
}

void disp_block_separator(struct disp *d)
{
    int need = 2 - d->trail;
    FILE *out = disp_sink(d);
    for (int i = 0; i < need; i++)
        fputc('\n', out);
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

/* All three eager strip variants share the same envelope: the quiet
 * chrome style, the box-drawing glyph and trailing space land in it,
 * then ANSI_RESET clears everything so callers can apply their own SGR
 * to the content that follows. Composed per call because the style
 * comes from the active theme. */
static void emit_strip(struct disp *d, const char *glyph_utf8)
{
    char strip[48];
    int n = snprintf(strip, sizeof(strip), "%s%s " ANSI_RESET, theme_open(THEME_CHROME_DIM),
                     glyph_utf8);
    disp_write(d, strip, (size_t)n);
}

void disp_tool_strip(struct disp *d)
{
    emit_strip(d, "\xE2\x94\x82"); /* │ U+2502 */
}

void disp_tool_strip_first(struct disp *d)
{
    emit_strip(d, "\xE2\x94\x8C"); /* ┌ U+250C */
}

void disp_tool_strip_solo(struct disp *d)
{
    emit_strip(d, "\xE2\x80\xBA"); /* › U+203A */
}

/* Shared overprint: \r back to col 0 of the current row, redraw the
 * leading glyph in quiet chrome, reset SGR. The "┌" or "│" originally
 * there is replaced by `glyph_utf8` (3-byte UTF-8 expected, single
 * cell). The space at col 1 and content from col 2 onward survive
 * untouched. Cursor lands at col 1; the next held-\n flush moves
 * down to a fresh row. */
static void tool_strip_overprint(struct disp *d, const char *glyph_utf8)
{
    FILE *out = disp_sink(d);
    fputs("\r", out);
    fputs(theme_open(THEME_CHROME_DIM), out);
    fputs(glyph_utf8, out);
    fputs(ANSI_RESET, out);
    fflush(out);
}

void disp_tool_strip_close(struct disp *d)
{
    tool_strip_overprint(d, "\xE2\x94\x94"); /* └ U+2514 */
}

void disp_tool_strip_close_solo(struct disp *d)
{
    tool_strip_overprint(d, "\xE2\x80\xBA"); /* › U+203A */
}
