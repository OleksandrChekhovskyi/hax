/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_FMT_H
#define HAX_TEXT_FMT_H

#include <stddef.h>

/* Parse a complete base-10 integer into out. Returns 1 on success and 0 otherwise. */
int parse_int(const char *str, int *out);

/* Round to seconds and format compactly, omitting zero remainders ("10m", "2h"); non-positive
 * values produce "0s". */
void format_duration(char *out, size_t out_size, long duration_ms);
/* As format_duration, but zero remainders stay ("10m 00s"), so a display that repaints in
 * place never shrinks and regrows at a unit boundary while ticking. */
void format_duration_steady(char *out, size_t out_size, long duration_ms);
/* Use more decimal places for sub-dollar values; non-positive values produce "$0.00". */
void format_cost(char *out, size_t out_size, double usd);

#endif /* HAX_TEXT_FMT_H */
