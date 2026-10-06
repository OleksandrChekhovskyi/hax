/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_USAGE_RENDER_H
#define HAX_PROVIDERS_USAGE_RENDER_H

#include <stddef.h>
#include <time.h>

/* Shared rendering for /usage reports, so every provider's report aligns the same way. */

/* Label column width shared by window and value rows. */
#define USAGE_LABEL_WIDTH 7

struct usage_window {
    const char *label;   /* row label, e.g. "weekly"; borrowed */
    double used_percent; /* 0-100; out-of-range values are clamped */
    time_t reset_at;
    const char *note; /* trailing marker, e.g. a non-ok status; borrowed, NULL for none */
};

/* Print the report heading on stdout: `name`, then the `n_details` members of `details` that are
 * neither NULL nor empty, dot-separated. Details are stripped of terminal controls, so server
 * strings are safe to pass. */
void usage_heading_print(const char *name, const char *const *details, size_t n_details);

/* Print `window` on stdout as one indented row: label, usage bar, percent, reset time.
 * Label and note are stripped of terminal controls, so server strings are safe to pass. */
void usage_window_print(const struct usage_window *window);

/* Print one indented row on stdout: `label` in the window label column, then the formatted value.
 * The value is printed as given; strip server text before formatting it in. */
__attribute__((format(printf, 2, 3))) void usage_value_print(const char *label, const char *fmt,
                                                             ...);

#endif /* HAX_PROVIDERS_USAGE_RENDER_H */
