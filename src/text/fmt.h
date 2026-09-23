/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_FMT_H
#define HAX_TEXT_FMT_H

#include <stddef.h>
#include <stdint.h>

/* Parse a complete base-10 integer into out. Returns 1 on success and 0 otherwise. */
int parse_int(const char *str, int *out);
/* Parse exactly count hex digits, one to eight, from a buffer with at least count readable bytes.
 * Returns 1 on success and 0 otherwise, leaving out unchanged on failure. */
int parse_hex(const char *digits, size_t count, uint32_t *out);

/* Round to seconds and format compactly, omitting zero remainders ("10m", "2h"); non-positive
 * values produce "0s". */
void format_duration(char *out, size_t out_size, long duration_ms);
/* As format_duration, but zero remainders stay ("10m 00s"), so a display that repaints in
 * place never shrinks and regrows at a unit boundary while ticking. */
void format_duration_steady(char *out, size_t out_size, long duration_ms);
/* Use more decimal places for sub-dollar values; non-positive values produce "$0.00". */
void format_cost(char *out, size_t out_size, double usd);
/* Smallest cost worth printing next to a token count; smaller estimates are omitted. */
#define COST_DISPLAY_MIN 0.00005

/* Use decimal k/M suffixes for token counts — tokens are specified and billed in decimal
 * multiples, unlike bytes. Negative values produce "?". */
void format_tokens(char *out, size_t out_size, long tokens);
/* Include the usage percentage when context_limit is positive; negative context_tokens means
 * unknown usage ("? / 256k", no percentage). */
void format_context(char *out, size_t out_size, long context_tokens, long context_limit);

#endif /* HAX_TEXT_FMT_H */
