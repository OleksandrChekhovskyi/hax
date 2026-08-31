/* SPDX-License-Identifier: MIT */
#include "text/fmt.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

int parse_int(const char *str, int *out)
{
    if (!str || !*str)
        return 0;

    char *end;
    errno = 0;
    long value = strtol(str, &end, 10);
    if (end == str || *end != '\0')
        return 0;
    if (errno == ERANGE || value > INT_MAX || value < INT_MIN)
        return 0;
    *out = (int)value;
    return 1;
}

void format_duration(char *out, size_t out_size, long duration_ms)
{
    long seconds = 0;
    if (duration_ms > 0)
        seconds = duration_ms / 1000 + (duration_ms % 1000 >= 500);

    if (seconds < 60)
        snprintf(out, out_size, "%lds", seconds);
    else if (seconds < 3600 && seconds % 60 == 0)
        snprintf(out, out_size, "%ldm", seconds / 60);
    else if (seconds < 3600)
        snprintf(out, out_size, "%ldm %02lds", seconds / 60, seconds % 60);
    else if (seconds % 3600 == 0)
        snprintf(out, out_size, "%ldh", seconds / 3600);
    else
        snprintf(out, out_size, "%ldh %02ldm", seconds / 3600, seconds % 3600 / 60);
}

void format_duration_steady(char *out, size_t out_size, long duration_ms)
{
    long seconds = 0;
    if (duration_ms > 0)
        seconds = duration_ms / 1000 + (duration_ms % 1000 >= 500);

    if (seconds < 60)
        snprintf(out, out_size, "%lds", seconds);
    else if (seconds < 3600)
        snprintf(out, out_size, "%ldm %02lds", seconds / 60, seconds % 60);
    else
        snprintf(out, out_size, "%ldh %02ldm", seconds / 3600, seconds % 3600 / 60);
}

void format_cost(char *out, size_t out_size, double usd)
{
    if (usd <= 0)
        snprintf(out, out_size, "$0.00");
    else if (usd < 0.01)
        snprintf(out, out_size, "$%.4f", usd);
    else if (usd < 1.0)
        snprintf(out, out_size, "$%.3f", usd);
    else
        snprintf(out, out_size, "$%.2f", usd);
}
