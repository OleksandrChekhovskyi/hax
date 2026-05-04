/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROGRESS_H
#define HAX_PROGRESS_H

/* Render a single-line progress bar to stdout (no trailing newline).
 *
 * `frac` is clamped to [0,1]; `width` is the bar's cell count.
 * Whole-cell resolution — `frac * width` rounds to the nearest cell.
 * The whole bar is drawn at ANSI_DIM with the fill/empty contrast
 * coming from glyph density (full vs light-shade block in UTF-8;
 * '#'/'-' in the ASCII fallback). Quiet by design, intended for
 * status output that should recede behind conversation text. */
void progress_bar_print(double frac, int width);

#endif /* HAX_PROGRESS_H */
