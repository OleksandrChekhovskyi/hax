/* SPDX-License-Identifier: MIT */
#ifndef HAX_SPINNER_H
#define HAX_SPINNER_H

/* A single-line "Working..." indicator animated by a background thread. All
 * entry points are idempotent and safe to call when stdout is not a TTY (in
 * which case the spinner becomes a no-op and no thread is created). */

struct spinner;

struct spinner *spinner_new(const char *label);
void spinner_show(struct spinner *s);
void spinner_hide(struct spinner *s);
/* Swap the label shown beside the glyph. If the spinner is currently
 * visible the new label is repainted on the same line; otherwise the
 * change just takes effect on the next show. Idempotent — passing the
 * current label is a no-op. NULL/"" reverts to the default ("working..."). */
void spinner_set_label(struct spinner *s, const char *label);
void spinner_free(struct spinner *s);

#endif /* HAX_SPINNER_H */
