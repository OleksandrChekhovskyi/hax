/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "tools/image_sniff.h"

/* Byte arrays keep the parsed format fields visible beside their assertions. */

static void test_png_dimensions(void)
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

static void test_gif_dimensions(void)
{
    const unsigned char gif[] = {'G',  'I', 'F', '8', '9', 'a', 0x40, 0x01, /* width 320 LE */
                                 0xc8, 0x00};                               /* height 200 LE */
    struct image_info info;
    EXPECT(image_sniff(gif, sizeof(gif), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/gif");
    EXPECT(info.width == 320);
    EXPECT(info.height == 200);
}

static void test_jpeg_dimensions(void)
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

static void test_webp_lossy_dimensions(void)
{
    const unsigned char webp[] = {
        'R',  'I',  'F',  'F',  0x16, 0x00, 0x00, 0x00, 'W',  'E',
        'B',  'P',  'V',  'P',  '8',  ' ',  0x0a, 0x00, 0x00, 0x00, /* chunk header */
        0x00, 0x00, 0x00, 0x9d, 0x01, 0x2a,                         /* frame tag + sync code */
        0x80, 0x02, 0xe0, 0x01,                                     /* 640x480 */
    };
    struct image_info info;
    EXPECT(image_sniff(webp, sizeof(webp), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/webp");
    EXPECT(info.width == 640);
    EXPECT(info.height == 480);
}

static void test_webp_lossless_dimensions(void)
{
    /* VP8L: 100x50 → width-1=99, height-1=49, packed 14+14 bits. */
    unsigned long bits = 99UL | (49UL << 14);
    const unsigned char webp[] = {
        'R',
        'I',
        'F',
        'F',
        0x12,
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
        0x00, /* RIFF padding */
    };
    struct image_info info;
    EXPECT(image_sniff(webp, sizeof(webp), &info) == 1);
    EXPECT_STR_EQ(info.mime, "image/webp");
    EXPECT(info.width == 100);
    EXPECT(info.height == 50);
}

static void test_webp_extended_dimensions(void)
{
    /* VP8X: canvas 1920x1080 as 24-bit (dim - 1). */
    const unsigned char webp[] = {
        'R',  'I',  'F',  'F',  0x16, 0x00, 0x00, 0x00, 'W',  'E',
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

static void expect_complete(const void *data, size_t data_len, int expected)
{
    struct image_info info;
    EXPECT(image_sniff(data, data_len, &info) == 1);
    EXPECT(info.complete == expected);
}

static void test_png_completeness(void)
{
    const unsigned char ihdr[] = {0x89, 'P',  'N',  'G',  0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
                                  0x0d, 'I',  'H',  'D',  'R',  0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
                                  0x00, 0x08, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const unsigned char idat[] = {0x00, 0x00, 0x00, 0x02, 'I',  'D',  'A',
                                  'T',  0x78, 0x9c, 0x00, 0x00, 0x00, 0x00};
    const unsigned char iend[] = {0x00, 0x00, 0x00, 0x00, 'I',  'E',
                                  'N',  'D',  0xae, 0x42, 0x60, 0x82};
    unsigned char png[sizeof(ihdr) + sizeof(idat) + sizeof(iend)];
    memcpy(png, ihdr, sizeof(ihdr));
    memcpy(png + sizeof(ihdr), idat, sizeof(idat));
    memcpy(png + sizeof(ihdr) + sizeof(idat), iend, sizeof(iend));

    expect_complete(png, sizeof(png), 1);
    expect_complete(png, sizeof(png) - 4, 0);
    expect_complete(ihdr, sizeof(ihdr), 0);

    unsigned char no_idat[sizeof(ihdr) + sizeof(iend)];
    memcpy(no_idat, ihdr, sizeof(ihdr));
    memcpy(no_idat + sizeof(ihdr), iend, sizeof(iend));
    expect_complete(no_idat, sizeof(no_idat), 0);

    unsigned char no_ihdr[8 + sizeof(idat) + sizeof(iend)];
    memcpy(no_ihdr, ihdr, 8);
    memcpy(no_ihdr + 8, idat, sizeof(idat));
    memcpy(no_ihdr + 8 + sizeof(idat), iend, sizeof(iend));
    expect_complete(no_ihdr, sizeof(no_ihdr), 0);
}

static void test_gif_completeness(void)
{
    const unsigned char gif[] = {
        'G',  'I',  'F',  '8',  '9',  'a',                          /* signature */
        0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,                   /* logical screen */
        0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, /* image descriptor */
        0x02, 0x02, 0x44, 0x01, 0x00,                               /* image data */
        0x3b,                                                       /* trailer */
    };
    expect_complete(gif, sizeof(gif), 1);
    expect_complete(gif, sizeof(gif) - 1, 0);

    const unsigned char no_image[] = {'G',  'I',  'F',  '8',  '9',  'a',  0x01,
                                      0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x3b};
    expect_complete(no_image, sizeof(no_image), 0);

    const unsigned char no_image_data[] = {
        'G',  'I',  'F',  '8',  '9',  'a',                          /* signature */
        0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,                   /* logical screen */
        0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, /* image descriptor */
        0x3b,
    };
    expect_complete(no_image_data, sizeof(no_image_data), 0);
}

static void test_jpeg_completeness(void)
{
    const unsigned char jpeg[] = {
        0xff, 0xd8,                                                       /* SOI */
        0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08, 0x03, 0x01, /* SOF0 8x8 */
        0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xda, 0x00,
        0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03,                   /* SOS */
        0x11, 0x00, 0x3f, 0x00, 0x12, 0x34, 0xff, 0x00, 0xff, 0xd0, /* entropy + restart */
        0x56, 0xff, 0xff, 0xd9,                                     /* fill byte + EOI */
    };
    expect_complete(jpeg, sizeof(jpeg), 1);
    expect_complete(jpeg, sizeof(jpeg) - 2, 0);

    const unsigned char no_scan[] = {0xff, 0xd8, 0xff, 0xd9};
    expect_complete(no_scan, sizeof(no_scan), 0);

    unsigned char duplicate_soi[sizeof(jpeg) + 2];
    memcpy(duplicate_soi, jpeg, 2);
    duplicate_soi[2] = 0xff;
    duplicate_soi[3] = 0xd8;
    memcpy(duplicate_soi + 4, jpeg + 2, sizeof(jpeg) - 2);
    expect_complete(duplicate_soi, sizeof(duplicate_soi), 0);

    /* An EOI inside APP1 belongs to its embedded thumbnail, not the main marker chain. */
    const unsigned char embedded_thumbnail[] = {
        0xff, 0xd8,                                                       /* SOI */
        0xff, 0xe1, 0x00, 0x08, 0xff, 0xd8, 0xff, 0xd9, 0x00, 0x00,       /* APP1 */
        0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x08, 0x03, 0x01, /* SOF0 8x8 */
        0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xda, 0x00,
        0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, /* SOS */
        0x11, 0x00, 0x3f, 0x00, 0x12, 0x34,
    };
    expect_complete(embedded_thumbnail, sizeof(embedded_thumbnail), 0);
}

static void test_webp_completeness(void)
{
    const unsigned char webp[] = {'R',  'I',  'F',  'F',  0x12, 0x00, 0x00, 0x00, 'W',
                                  'E',  'B',  'P',  'V',  'P',  '8',  'L',  0x05, 0x00,
                                  0x00, 0x00, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x00};
    expect_complete(webp, sizeof(webp), 1);
    expect_complete(webp, sizeof(webp) - 1, 0);

    unsigned char invalid_chunk_len[sizeof(webp)];
    memcpy(invalid_chunk_len, webp, sizeof(webp));
    invalid_chunk_len[16] = 0x07;
    expect_complete(invalid_chunk_len, sizeof(invalid_chunk_len), 0);
}

static void test_webp_empty_image_chunks(void)
{
    const unsigned char empty_vp8[] = {'R', 'I', 'F', 'F', 0x0c, 0x00, 0x00, 0x00, 'W',  'E',
                                       'B', 'P', 'V', 'P', '8',  ' ',  0x00, 0x00, 0x00, 0x00};
    expect_complete(empty_vp8, sizeof(empty_vp8), 0);

    const unsigned char empty_vp8l[] = {'R', 'I', 'F', 'F', 0x0c, 0x00, 0x00, 0x00, 'W',  'E',
                                        'B', 'P', 'V', 'P', '8',  'L',  0x00, 0x00, 0x00, 0x00};
    expect_complete(empty_vp8l, sizeof(empty_vp8l), 0);

    const unsigned char empty_anmf[] = {
        'R',  'I',  'F',  'F',  0x1e, 0x00, 0x00, 0x00, 'W',  'E',
        'B',  'P',  'V',  'P',  '8',  'X',  0x0a, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 1x1 animated canvas */
        'A',  'N',  'M',  'F',  0x00, 0x00, 0x00, 0x00,             /* empty frame */
    };
    struct image_info info;
    EXPECT(image_sniff(empty_anmf, sizeof(empty_anmf), &info) == 1);
    EXPECT(info.width == 1 && info.height == 1);
    EXPECT(info.complete == 0);
}

static void test_webp_animation_completeness(void)
{
    const unsigned char webp[] = {
        'R',  'I',  'F',  'F',  0x3c, 0x00, 0x00, 0x00, 'W',  'E',
        'B',  'P',  'V',  'P',  '8',  'X',  0x0a, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 1x1 animated canvas */
        'A',  'N',  'M',  'F',  0x1e, 0x00, 0x00, 0x00,             /* frame chunk */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* frame position + size */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* size + duration + flags */
        'V',  'P',  '8',  'L',  0x05, 0x00, 0x00, 0x00, 0x2f, 0x00,
        0x00, 0x00, 0x00, 0x00, /* payload + padding */
    };
    expect_complete(webp, sizeof(webp), 1);
}

static void test_not_an_image(void)
{
    struct image_info info = {.mime = "stale", .width = 1, .height = 1, .complete = 1};
    const char text[] = "#!/bin/sh\necho hello\n";
    EXPECT(image_sniff(text, sizeof(text) - 1, &info) == 0);
    EXPECT(info.mime == NULL && info.width == 0 && info.height == 0 && info.complete == 0);
    const unsigned char elf[] = {0x7f, 'E', 'L', 'F', 0x02, 0x01, 0x01, 0x00};
    EXPECT(image_sniff(elf, sizeof(elf), &info) == 0);
    EXPECT(image_sniff("", 0, &info) == 0);
    /* RIFF that isn't WebP (e.g. WAV) must not match. */
    const unsigned char wav[] = {'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E'};
    EXPECT(image_sniff(wav, sizeof(wav), &info) == 0);
}

int main(void)
{
    test_png_dimensions();
    test_gif_dimensions();
    test_jpeg_dimensions();
    test_webp_lossy_dimensions();
    test_webp_lossless_dimensions();
    test_webp_extended_dimensions();
    test_png_completeness();
    test_gif_completeness();
    test_jpeg_completeness();
    test_webp_completeness();
    test_webp_empty_image_chunks();
    test_webp_animation_completeness();
    test_not_an_image();
    T_REPORT();
}
