/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_USAGE_RENDER_H
#define HAX_PROVIDERS_USAGE_RENDER_H

#include <time.h>

/* Shared rendering for /usage rate-limit rows, so every provider's report aligns the same way. */

/* Label column width; auxiliary lines printed among windows should align to it. */
#define USAGE_LABEL_WIDTH 7

struct usage_window {
    const char *label;   /* row label, e.g. "weekly"; borrowed */
    double used_percent; /* 0-100; out-of-range values are clamped */
    time_t reset_at;
    const char *note; /* trailing marker, e.g. a non-ok status; borrowed, NULL for none */
};

/* Print `window` on stdout as one indented row: label, usage bar, percent, reset time.
 * Label and note are stripped of terminal controls, so server strings are safe to pass. */
void usage_window_print(const struct usage_window *window);

#endif /* HAX_PROVIDERS_USAGE_RENDER_H */
