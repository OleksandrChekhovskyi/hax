/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_IMAGE_SNIFF_H
#define HAX_TOOLS_IMAGE_SNIFF_H

#include <stddef.h>

struct image_info {
    const char *mime; /* borrowed static string; do not free */
    long width;       /* pixels; 0 when unavailable */
    long height;      /* pixels; 0 when unavailable */
    int complete;
};

/* Detects PNG, JPEG, GIF, or WebP from content and always initializes `info`. Returns 1 for a
 * recognized signature and 0 otherwise; classification reads at most the first 12 bytes. Dimensions
 * are best-effort, and JPEG input must extend through its first frame header to provide them.
 * `complete` is meaningful only when `buf` contains the whole file and validates framing, not
 * compressed image data or checksums. */
int image_sniff(const void *buf, size_t buf_len, struct image_info *info);

#endif /* HAX_TOOLS_IMAGE_SNIFF_H */
