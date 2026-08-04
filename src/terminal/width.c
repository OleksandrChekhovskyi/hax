/* SPDX-License-Identifier: MIT */
#include "terminal/width.h"

#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "config.h"

int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 120;
}

int display_width(void)
{
    const char *mode = config_str("display_width");
    int configured_width = config_int("display_width");
    if (configured_width >= 20)
        return configured_width;

    int width = term_width();
    if ((!mode || strcasecmp(mode, "terminal") != 0) && width > DISPLAY_WIDTH_CAP)
        width = DISPLAY_WIDTH_CAP;
    return width < 20 ? 20 : width;
}

int reflow_physical_rows(const int *row_widths, int count, int terminal_cols)
{
    int physical_rows = 0;
    for (int row = 0; row < count; row++) {
        int width = row_widths[row];
        physical_rows += width > 0 ? (width + terminal_cols - 1) / terminal_cols : 1;
    }
    return physical_rows;
}
