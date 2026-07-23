/* SPDX-License-Identifier: MIT */
#include "tools/image_sniff.h"

#include <stdint.h>
#include <string.h>

static long be32(const unsigned char *p)
{
    return ((long)p[0] << 24) | ((long)p[1] << 16) | ((long)p[2] << 8) | p[3];
}

static long be16(const unsigned char *p)
{
    return ((long)p[0] << 8) | p[1];
}

static long le16(const unsigned char *p)
{
    return ((long)p[1] << 8) | p[0];
}

static long le24(const unsigned char *p)
{
    return ((long)p[2] << 16) | ((long)p[1] << 8) | p[0];
}

/* Walk the JPEG marker chain to the first SOFn frame header, which
 * carries the dimensions. Entropy-coded data only follows SOS, and the
 * frame header precedes it in every baseline/progressive file, so the
 * walk stays cheap even on large files. */
static void jpeg_dimensions(const unsigned char *p, size_t n, struct image_info *out)
{
    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xff) /* not a marker — corrupt chain; give up */
            return;
        unsigned char m = p[i + 1];
        if (m == 0xff) { /* fill byte */
            i++;
            continue;
        }
        /* Standalone markers without a length field. */
        if (m == 0x01 || (m >= 0xd0 && m <= 0xd7)) {
            i += 2;
            continue;
        }
        if (m == 0xd9 || m == 0xda) /* EOI / SOS: no frame header seen */
            return;
        size_t len = (size_t)be16(p + i + 2);
        if (len < 2)
            return;
        /* SOF0..SOF15 minus the non-frame members of the range (DHT,
         * JPG, DAC). Payload: precision u8, height u16, width u16. */
        if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            if (i + 9 <= n) {
                out->height = be16(p + i + 5);
                out->width = be16(p + i + 7);
            }
            return;
        }
        i += 2 + len;
    }
}

/* True if the JPEG's marker chain terminates in an EOI after its final
 * scan. Walking length-delimited segments (rather than scanning for FF D9
 * anywhere) skips APPn metadata wholesale, so an EXIF/MPF thumbnail's own
 * embedded EOI is never mistaken for the main image's — a scan truncated
 * after SOF stays flagged incomplete. No decoding: only marker framing. */
static int jpeg_complete(const unsigned char *p, size_t n)
{
    size_t i = 2;
    while (i + 1 < n) {
        if (p[i] != 0xff) { /* not at a marker; stray byte between segments */
            i++;
            continue;
        }
        unsigned char m = p[i + 1];
        if (m == 0xff) { /* fill byte */
            i++;
            continue;
        }
        if (m == 0xd9) /* EOI: the chain terminated */
            return 1;
        if (m == 0x01 || (m >= 0xd0 && m <= 0xd7)) { /* standalone, no length */
            i += 2;
            continue;
        }
        if (i + 4 > n)
            return 0;
        size_t len = (size_t)be16(p + i + 2);
        if (len < 2)
            return 0;
        i += 2 + len; /* skip the length-delimited segment header/payload */
        if (m == 0xda) {
            /* SOS: entropy-coded data follows with no length. Scan past
             * byte-stuffing (FF 00) and restart markers to the next real
             * marker, then resume the walk (EOI, or another scan). */
            while (i + 1 < n) {
                if (p[i] != 0xff) {
                    i++;
                    continue;
                }
                unsigned char e = p[i + 1];
                if (e == 0x00 || (e >= 0xd0 && e <= 0xd7) || e == 0xff) {
                    i += (e == 0xff) ? 1 : 2;
                    continue;
                }
                break;
            }
        }
    }
    return 0;
}

/* Walk the PNG chunk chain, requiring both an IDAT (pixel data) and a
 * terminating IEND. A signature + IHDR + IEND with no IDAT is a structurally
 * invalid file a decoder rejects; catching it here keeps such a payload out
 * of history. CRCs are not verified — that would be decoding, and corrupt
 * data can only be caught by the decoder itself. */
static int png_complete(const unsigned char *p, size_t n)
{
    size_t i = 8; /* past the 8-byte signature */
    int saw_idat = 0;
    while (i + 8 <= n) { /* room for a chunk's length + type */
        size_t len = (size_t)be32(p + i);
        const unsigned char *type = p + i + 4;
        if (memcmp(type, "IDAT", 4) == 0)
            saw_idat = 1;
        else if (memcmp(type, "IEND", 4) == 0)
            return saw_idat;
        if (i + 12 > n || len > n - (i + 12)) /* data + CRC run past the buffer */
            return 0;
        i += 12 + len; /* 4 len + 4 type + len data + 4 CRC */
    }
    return 0;
}

/* True if the GIF block stream contains an image descriptor (0x2C) — the
 * marker introducing pixel data. A header + trailer with no image passes the
 * dimension and trailer checks but is not a displayable image. Skips the
 * global color table and any extension blocks to reach the descriptor. */
static int gif_has_image(const unsigned char *p, size_t n)
{
    if (n < 13)
        return 0;
    size_t i = 13; /* past signature (6) + logical screen descriptor (7) */
    if (p[10] & 0x80)
        i += (size_t)3 << ((p[10] & 0x07) + 1); /* global color table */
    while (i < n) {
        if (p[i] == 0x2c) /* image descriptor: pixel data follows */
            return 1;
        if (p[i] == 0x3b) /* trailer: end of stream, no image */
            return 0;
        if (p[i] != 0x21) /* neither image, trailer, nor extension → malformed */
            return 0;
        i += 2; /* extension introducer + label */
        while (i < n && p[i])
            i += 1 + p[i]; /* length-prefixed sub-block */
        i += 1;            /* block terminator (0x00) */
    }
    return 0;
}

int image_sniff(const void *buf, size_t n, struct image_info *out)
{
    const unsigned char *p = buf;
    out->mime = NULL;
    out->width = 0;
    out->height = 0;
    out->complete = 0;

    static const unsigned char PNG_SIG[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n >= 8 && memcmp(p, PNG_SIG, 8) == 0) {
        out->mime = "image/png";
        /* IHDR is mandated first: 8 sig + 4 len + "IHDR", then w/h. */
        if (n >= 24 && memcmp(p + 12, "IHDR", 4) == 0) {
            out->width = be32(p + 16);
            out->height = be32(p + 20);
        }
        out->complete = png_complete(p, n);
        return 1;
    }

    if (n >= 6 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) {
        out->mime = "image/gif";
        if (n >= 10) {
            out->width = le16(p + 6);
            out->height = le16(p + 8);
        }
        /* Trailer byte present AND an actual image descriptor in the stream. */
        out->complete = n >= 10 && p[n - 1] == 0x3b && gif_has_image(p, n);
        return 1;
    }

    if (n >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) {
        out->mime = "image/jpeg";
        jpeg_dimensions(p, n, out);
        out->complete = jpeg_complete(p, n);
        return 1;
    }

    if (n >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) {
        out->mime = "image/webp";
        /* RIFF size (bytes 4..7, LE) counts everything after it; if that
         * plus the 8-byte header outruns the buffer, the file is truncated. */
        out->complete = (size_t)((uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) |
                                 ((uint32_t)p[7] << 24)) +
                            8 <=
                        n;
        const unsigned char *c = p + 12; /* first chunk header */
        const unsigned char *q = p + 20; /* first chunk payload */
        if (n >= 30 && memcmp(c, "VP8 ", 4) == 0) {
            /* Lossy: 3-byte frame tag, sync code, then 14-bit dims. */
            if (q[3] == 0x9d && q[4] == 0x01 && q[5] == 0x2a) {
                out->width = le16(q + 6) & 0x3fff;
                out->height = le16(q + 8) & 0x3fff;
            }
        } else if (n >= 25 && memcmp(c, "VP8L", 4) == 0) {
            /* Lossless: signature byte then 14+14 bits of (dim - 1). */
            if (q[0] == 0x2f) {
                uint32_t bits = (uint32_t)q[1] | ((uint32_t)q[2] << 8) | ((uint32_t)q[3] << 16) |
                                ((uint32_t)q[4] << 24);
                out->width = (long)(bits & 0x3fff) + 1;
                out->height = (long)((bits >> 14) & 0x3fff) + 1;
            }
        } else if (n >= 30 && memcmp(c, "VP8X", 4) == 0) {
            /* Extended: flags u8, reserved x3, then 24-bit (dim - 1). */
            out->width = le24(q + 4) + 1;
            out->height = le24(q + 7) + 1;
        }
        return 1;
    }

    return 0;
}
