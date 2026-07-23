/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_IMAGE_SNIFF_H
#define HAX_TOOLS_IMAGE_SNIFF_H

#include <stddef.h>

/* Magic-byte detection plus header dimension parsing for the image
 * formats every major model API accepts (PNG/JPEG/GIF/WebP). Detection
 * never trusts file extensions. */
struct image_info {
    const char *mime; /* static literal, never freed */
    long width;       /* pixels; 0 = not determined */
    long height;
    int complete; /* 1 = terminal structure present (see below) */
};

/* Returns 1 when `buf` starts with a known image signature (filling
 * *out), 0 otherwise. The signature check needs at most the first 16
 * bytes, so a short head-read suffices to classify. Dimensions are
 * best-effort: a recognized image whose header is truncated or exotic
 * yields 0x0, which callers treat as unknown rather than an error —
 * except JPEG, whose dimensions live in a SOF marker that can sit
 * anywhere in the segment chain, so pass the whole file when the
 * dimensions matter.
 *
 * `complete` is a cheap structural check (no decoding): it is set when the
 * format's mandatory image data and terminal structure are present in `buf`
 * — a PNG with both IDAT and IEND, a GIF with an image descriptor and
 * trailer, a JPEG whose scan reaches EOI, or a WebP whose RIFF size fits —
 * so it is only meaningful when the whole file was passed. A recognized but
 * incomplete image (a partial download, or a header with no pixel data) is
 * refused rather than shipped to a provider that would reject it. It does
 * not verify pixel data itself; only a decoder can catch corrupt contents. */
int image_sniff(const void *buf, size_t n, struct image_info *out);

#endif /* HAX_TOOLS_IMAGE_SNIFF_H */
