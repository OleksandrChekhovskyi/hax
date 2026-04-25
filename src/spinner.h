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
void spinner_free(struct spinner *s);

#endif /* HAX_SPINNER_H */
