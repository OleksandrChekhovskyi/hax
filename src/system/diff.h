/* SPDX-License-Identifier: MIT */
#ifndef HAX_DIFF_H
#define HAX_DIFF_H

#include <stddef.h>

/*
 * Generate a unified diff between two in-memory buffers by shelling out to
 * diff -u (resolved via PATH). POSIX-mandated and preinstalled almost
 * everywhere, but not guaranteed: minimal Arch installs lack diffutils, so
 * the README lists it as a dependency. No in-house diff algorithm to
 * maintain.
 *
 * a_label / b_label are the strings emitted on the `--- ` / `+++ ` header
 * lines (e.g. "a/path", "b/path", or "/dev/null" for absent files).
 *
 * Returns a freshly allocated NUL-terminated diff string. Empty string ("")
 * means the inputs are byte-identical. Returns NULL on internal error
 * (mkstemp / fork / diff exited with status > 1); caller handles that as
 * tool failure.
 */
char *make_unified_diff(const char *a, size_t a_len, const char *b, size_t b_len,
                        const char *a_label, const char *b_label);

#endif /* HAX_DIFF_H */
