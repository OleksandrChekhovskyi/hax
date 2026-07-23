/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "tools/image_sniff.h"

/* Minimal-but-valid headers, hand-assembled so the tests document the
 * byte layout each parser relies on. */

static void test_png(void)
{
    /* Signature + IHDR chunk: 800x600. */
    const unsigned char png[] = {0x89, 'P',  'N',  'G',  0x0d, 0x0a, 0x1a, 0x0a, /* sig */
                                 0x00, 0x00, 0x00, 0x0d, 'I',  'H',  'D',  'R',  /* len + tag */
                                 0x00, 0x00, 0x03, 0x20,                         /* width 800 */
                                 0x00, 0x00, 0x02, 0x58,                         /* height 600 */
                                 0x08, 0x06, 0x00, 0x00, 0x00};
    struct image_info info;
    EXPECT(image_sniff(png, sizeof(png), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/png");
    EXPECT(info.width == 800);
    EXPECT(info.height == 600);

    /* Truncated to just the signature: recognized, dimensions unknown. */
    EXPECT(image_sniff(png, 8, &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/png");
    EXPECT(info.width == 0 && info.height == 0);
}

static void test_gif(void)
{
    const unsigned char gif[] = {'G',  'I', 'F', '8', '9', 'a', 0x40, 0x01, /* width 320 LE */
                                 0xc8, 0x00};                               /* height 200 LE */
    struct image_info info;
    EXPECT(image_sniff(gif, sizeof(gif), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/gif");
    EXPECT(info.width == 320);
    EXPECT(info.height == 200);
}

static void test_jpeg(void)
{
    /* SOI, APP0 (JFIF), SOF0 with 256x512, EOI. */
    const unsigned char jpg[] = {
        0xff, 0xd8,                                                 /* SOI */
        0xff, 0xe0, 0x00, 0x10, 'J',  'F',  'I',  'F',  0x00, 0x01, /* APP0 */
        0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,             /* ... */
        0xff, 0xc0, 0x00, 0x11, 0x08,                               /* SOF0, precision */
        0x01, 0x00,                                                 /* height 256 */
        0x02, 0x00,                                                 /* width 512 */
        0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, /* components */
        0xff, 0xd9,                                                 /* EOI */
    };
    struct image_info info;
    EXPECT(image_sniff(jpg, sizeof(jpg), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/jpeg");
    EXPECT(info.width == 512);
    EXPECT(info.height == 256);
}

static void test_webp_lossless(void)
{
    /* VP8L: 100x50 → width-1=99, height-1=49, packed 14+14 bits. */
    unsigned long bits = 99UL | (49UL << 14);
    const unsigned char webp[] = {
        'R',
        'I',
        'F',
        'F',
        0x11,
        0x00,
        0x00,
        0x00,
        'W',
        'E',
        'B',
        'P',
        'V',
        'P',
        '8',
        'L',
        0x05,
        0x00,
        0x00,
        0x00, /* chunk header */
        0x2f, /* signature */
        (unsigned char)(bits & 0xff),
        (unsigned char)((bits >> 8) & 0xff),
        (unsigned char)((bits >> 16) & 0xff),
        (unsigned char)((bits >> 24) & 0xff),
    };
    struct image_info info;
    EXPECT(image_sniff(webp, sizeof(webp), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/webp");
    EXPECT(info.width == 100);
    EXPECT(info.height == 50);
}

static void test_webp_extended(void)
{
    /* VP8X: canvas 1920x1080 as 24-bit (dim - 1). */
    const unsigned char webp[] = {
        'R',  'I',  'F',  'F',  0x12, 0x00, 0x00, 0x00, 'W',  'E',
        'B',  'P',  'V',  'P',  '8',  'X',  0x0a, 0x00, 0x00, 0x00, /* chunk header */
        0x00, 0x00, 0x00, 0x00,                                     /* flags + reserved */
        0x7f, 0x07, 0x00,                                           /* 1919 LE24 */
        0x37, 0x04, 0x00,                                           /* 1079 LE24 */
    };
    struct image_info info;
    EXPECT(image_sniff(webp, sizeof(webp), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/webp");
    EXPECT(info.width == 1920);
    EXPECT(info.height == 1080);
}

/* `complete` is the truncation guard: it is set only when the format's
 * terminal structure is present, so a partial download is recognized (mime
 * set) yet flagged incomplete. */
static void test_complete(void)
{
    struct image_info info;

    /* PNG chunk chain: IHDR (8x8) + IDAT + IEND. Completeness requires both
     * IDAT (pixel data) and IEND; CRCs are not checked, so zeros suffice. */
    const unsigned char ihdr[] = {0x89, 'P',  'N',  'G',  0x0d, 0x0a, 0x1a, 0x0a, /* signature */
                                  0x00, 0x00, 0x00, 0x0d, 'I',  'H',  'D',  'R',  /* len 13 + tag */
                                  0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, /* 8x8 */
                                  0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* + CRC */
    const unsigned char idat[] = {0x00, 0x00, 0x00, 0x02, 'I',  'D',  'A',
                                  'T',  0x78, 0x9c, 0x00, 0x00, 0x00, 0x00};
    const unsigned char iend[] = {0x00, 0x00, 0x00, 0x00, 'I',  'E',
                                  'N',  'D',  0xae, 0x42, 0x60, 0x82};
    unsigned char png[sizeof(ihdr) + sizeof(idat) + sizeof(iend)];
    memcpy(png, ihdr, sizeof(ihdr));
    memcpy(png + sizeof(ihdr), idat, sizeof(idat));
    memcpy(png + sizeof(ihdr) + sizeof(idat), iend, sizeof(iend));
    EXPECT(image_sniff(png, sizeof(png), &info) == 1 && info.complete == 1);

    /* IHDR without IDAT (even with IEND) is structurally invalid → incomplete. */
    unsigned char no_idat[sizeof(ihdr) + sizeof(iend)];
    memcpy(no_idat, ihdr, sizeof(ihdr));
    memcpy(no_idat + sizeof(ihdr), iend, sizeof(iend));
    EXPECT(image_sniff(no_idat, sizeof(no_idat), &info) == 1 && info.complete == 0);
    /* Truncated before IEND is likewise incomplete. */
    EXPECT(image_sniff(ihdr, sizeof(ihdr), &info) == 1 && info.complete == 0);

    /* GIF: logical screen descriptor (no GCT) + image descriptor + trailer. */
    const unsigned char gif[] = {'G',  'I',  'F',  '8',  '9',  'a',        /* signature */
                                 0x0a, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, /* LSD 10x10, no GCT */
                                 0x2c, 0x00, 0x00, 0x00, 0x00,             /* image descriptor */
                                 0x3b};                                    /* trailer */
    EXPECT(image_sniff(gif, sizeof(gif), &info) == 1 && info.complete == 1);
    EXPECT(image_sniff(gif, sizeof(gif) - 1, &info) == 1 && info.complete == 0); /* no trailer */
    /* Header + trailer but no image descriptor: recognized yet incomplete. */
    const unsigned char gif_empty[] = {'G',  'I',  'F',  '8',  '9',  'a',  0x0a,
                                       0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x3b};
    EXPECT(image_sniff(gif_empty, sizeof(gif_empty), &info) == 1 && info.complete == 0);

    /* JPEG: SOF0 (8x8) + SOS + a short entropy stream (with a stuffed FF 00)
     * + EOI. The marker walk must reach the EOI past the entropy data. */
    const unsigned char jpg[] = {
        0xff, 0xd8,                                                       /* SOI */
        0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08, 0x03, 0x01, /* SOF0 8x8 */
        0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,                   /* ... */
        0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03,       /* SOS */
        0x11, 0x00, 0x3f, 0x00,                                           /* ... */
        0x12, 0x34, 0xff, 0x00, 0x56,                                     /* entropy */
        0xff, 0xd9,                                                       /* EOI */
    };
    EXPECT(image_sniff(jpg, sizeof(jpg), &info) == 1 && info.complete == 1);
    EXPECT(image_sniff(jpg, sizeof(jpg) - 2, &info) == 1 && info.complete == 0);

    /* Regression: an APP1 segment carrying an embedded thumbnail's own FF D9
     * must not count. Here the main scan has no EOI, so despite the thumbnail
     * marker the file is incomplete. */
    const unsigned char thumb[] = {
        0xff, 0xd8,                                                       /* SOI */
        0xff, 0xe1, 0x00, 0x08, 0xff, 0xd8, 0xff, 0xd9, 0x00, 0x00,       /* APP1 w/ thumb EOI */
        0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08, 0x03, 0x01, /* SOF0 8x8 */
        0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,                   /* ... */
        0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03,       /* SOS */
        0x11, 0x00, 0x3f, 0x00,                                           /* ... */
        0x12, 0x34,                                                       /* truncated entropy */
    };
    EXPECT(image_sniff(thumb, sizeof(thumb), &info) == 1 && info.complete == 0);

    /* WebP: RIFF size (17) + 8 must fit the buffer. */
    const unsigned char webp[] = {'R',  'I',  'F',  'F',  0x11, 0x00, 0x00, 0x00, 'W',
                                  'E',  'B',  'P',  'V',  'P',  '8',  'L',  0x05, 0x00,
                                  0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x00};
    EXPECT(image_sniff(webp, sizeof(webp), &info) == 1 && info.complete == 1);
    EXPECT(image_sniff(webp, sizeof(webp) - 1, &info) == 1 && info.complete == 0);
}

static void test_not_an_image(void)
{
    struct image_info info;
    const char text[] = "#!/bin/sh\necho hello\n";
    EXPECT(image_sniff(text, sizeof(text) - 1, &info) == 0);
    const unsigned char elf[] = {0x7f, 'E', 'L', 'F', 0x02, 0x01, 0x01, 0x00};
    EXPECT(image_sniff(elf, sizeof(elf), &info) == 0);
    EXPECT(image_sniff("", 0, &info) == 0);
    /* RIFF that isn't WebP (e.g. WAV) must not match. */
    const unsigned char wav[] = {'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E'};
    EXPECT(image_sniff(wav, sizeof(wav), &info) == 0);
}

int main(void)
{
    test_png();
    test_gif();
    test_jpeg();
    test_webp_lossless();
    test_webp_extended();
    test_complete();
    test_not_an_image();
    T_REPORT();
}
