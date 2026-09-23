/* SPDX-License-Identifier: MIT */
#include "text/fmt.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int parse_hex(const char *digits, size_t count, uint32_t *out)
{
    if (count < 1 || count > 8)
        return 0;
    uint32_t value = 0;
    for (size_t i = 0; i < count; i++) {
        char c = digits[i];
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = (uint32_t)(c - 'A' + 10);
        else
            return 0;
        value = value << 4 | digit;
    }
    *out = value;
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

/* "2.0k" reads as noise next to "412" and "2.5k"; print whole multiples bare. */
static void format_one_decimal(char *out, size_t out_size, double value, char suffix)
{
    snprintf(out, out_size, "%.1f%c", value, suffix);
    char *zero_fraction = strstr(out, ".0");
    if (zero_fraction)
        memmove(zero_fraction, zero_fraction + 2, strlen(zero_fraction + 2) + 1);
}

void format_tokens(char *out, size_t out_size, long tokens)
{
    const long million = 1000000L;
    if (tokens < 0)
        snprintf(out, out_size, "?");
    else if (tokens < 1000)
        snprintf(out, out_size, "%ld", tokens);
    else if (tokens < 10L * 1000)
        format_one_decimal(out, out_size, (double)tokens / 1000.0, 'k');
    else if (tokens < million)
        snprintf(out, out_size, "%ldk", tokens / 1000 + (tokens % 1000 >= 500));
    else if (tokens < 10L * million)
        format_one_decimal(out, out_size, (double)tokens / (double)million, 'M');
    else
        snprintf(out, out_size, "%ldM", tokens / million + (tokens % million >= million / 2));
}

void format_context(char *out, size_t out_size, long context_tokens, long context_limit)
{
    char used[32];
    format_tokens(used, sizeof(used), context_tokens);
    if (context_limit > 0 && context_tokens >= 0) {
        char limit[32];
        /* Usage above the window is real (stale model metadata), so report it rather than
         * capping at 100; the ceiling only keeps the field three digits wide. */
        double ratio = (double)context_tokens * 100.0 / (double)context_limit;
        long percentage = ratio > 999.0 ? 999 : (long)ratio;
        format_tokens(limit, sizeof(limit), context_limit);
        snprintf(out, out_size, "%s / %s (%ld%%)", used, limit, percentage);
    } else if (context_limit > 0) {
        char limit[32];
        format_tokens(limit, sizeof(limit), context_limit);
        snprintf(out, out_size, "%s / %s", used, limit);
    } else {
        snprintf(out, out_size, "%s", used);
    }
}
