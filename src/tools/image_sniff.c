/* SPDX-License-Identifier: MIT */
#include "tools/image_sniff.h"

#include <stdint.h>
#include <string.h>

enum jpeg_marker {
    JPEG_STUFFED_BYTE = 0x00,
    JPEG_TEM = 0x01,
    JPEG_SOF_MIN = 0xc0,
    JPEG_DHT = 0xc4,
    JPEG_JPG = 0xc8,
    JPEG_DAC = 0xcc,
    JPEG_SOF_MAX = 0xcf,
    JPEG_RST_MIN = 0xd0,
    JPEG_RST_MAX = 0xd7,
    JPEG_SOI = 0xd8,
    JPEG_EOI = 0xd9,
    JPEG_SOS = 0xda,
    JPEG_MARKER_PREFIX = 0xff,
};

enum gif_block_type {
    GIF_EXTENSION = 0x21,
    GIF_IMAGE_DESCRIPTOR = 0x2c,
    GIF_TRAILER = 0x3b,
};

enum gif_flag {
    GIF_COLOR_TABLE_SIZE_MASK = 0x07,
    GIF_COLOR_TABLE_FLAG = 0x80,
};

static uint16_t read_be16(const unsigned char *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t read_be32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) |
           data[3];
}

static uint16_t read_le16(const unsigned char *data)
{
    return ((uint16_t)data[1] << 8) | data[0];
}

static uint32_t read_le24(const unsigned char *data)
{
    return ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
}

static uint32_t read_le32(const unsigned char *data)
{
    return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) |
           data[0];
}

static int jpeg_marker_is_standalone(unsigned char marker)
{
    return marker == JPEG_TEM || (marker >= JPEG_RST_MIN && marker <= JPEG_RST_MAX);
}

static int jpeg_marker_is_sof(unsigned char marker)
{
    return marker >= JPEG_SOF_MIN && marker <= JPEG_SOF_MAX && marker != JPEG_DHT &&
           marker != JPEG_JPG && marker != JPEG_DAC;
}

static void read_jpeg_dimensions(const unsigned char *data, size_t data_len,
                                 struct image_info *info)
{
    size_t offset = 2;

    while (offset < data_len) {
        if (data[offset++] != JPEG_MARKER_PREFIX)
            return;
        while (offset < data_len && data[offset] == JPEG_MARKER_PREFIX)
            offset++;
        if (offset >= data_len)
            return;

        unsigned char marker = data[offset++];
        if (marker == JPEG_STUFFED_BYTE || marker == JPEG_EOI || marker == JPEG_SOS)
            return;
        if (jpeg_marker_is_standalone(marker))
            continue;
        if (data_len - offset < 2)
            return;

        size_t segment_len = read_be16(data + offset);
        if (segment_len < 2 || segment_len > data_len - offset)
            return;
        if (jpeg_marker_is_sof(marker)) {
            if (segment_len >= 7) {
                info->height = read_be16(data + offset + 3);
                info->width = read_be16(data + offset + 5);
            }
            return;
        }
        offset += segment_len;
    }
}

static int jpeg_is_complete(const unsigned char *data, size_t data_len)
{
    size_t offset = 2;
    int saw_scan = 0;

    while (offset < data_len) {
        if (data[offset++] != JPEG_MARKER_PREFIX)
            return 0;
        while (offset < data_len && data[offset] == JPEG_MARKER_PREFIX)
            offset++;
        if (offset >= data_len)
            return 0;

        unsigned char marker = data[offset++];
        if (marker == JPEG_STUFFED_BYTE)
            return 0;
        if (marker == JPEG_EOI)
            return saw_scan;
        if (jpeg_marker_is_standalone(marker))
            continue;
        if (data_len - offset < 2)
            return 0;

        size_t segment_len = read_be16(data + offset);
        if (segment_len < 2 || segment_len > data_len - offset)
            return 0;
        offset += segment_len;
        if (marker != JPEG_SOS)
            continue;

        saw_scan = 1;
        /* Entropy data uses FF 00 byte stuffing and permits restart markers. */
        while (offset + 1 < data_len) {
            if (data[offset] != JPEG_MARKER_PREFIX) {
                offset++;
                continue;
            }
            unsigned char escaped = data[offset + 1];
            if (escaped == JPEG_STUFFED_BYTE ||
                (escaped >= JPEG_RST_MIN && escaped <= JPEG_RST_MAX)) {
                offset += 2;
                continue;
            }
            if (escaped == JPEG_MARKER_PREFIX) {
                offset++;
                continue;
            }
            break;
        }
    }
    return 0;
}

static int png_is_complete(const unsigned char *data, size_t data_len)
{
    size_t offset = 8;
    int saw_ihdr = 0;
    int saw_image_data = 0;

    while (offset <= data_len) {
        size_t remaining = data_len - offset;
        if (remaining < 12)
            return 0;

        size_t chunk_data_len = read_be32(data + offset);
        if (chunk_data_len > remaining - 12)
            return 0;

        const unsigned char *chunk_type = data + offset + 4;
        if (!saw_ihdr) {
            if (memcmp(chunk_type, "IHDR", 4) != 0 || chunk_data_len != 13)
                return 0;
            saw_ihdr = 1;
        } else if (memcmp(chunk_type, "IDAT", 4) == 0) {
            saw_image_data = 1;
        } else if (memcmp(chunk_type, "IEND", 4) == 0) {
            return chunk_data_len == 0 && saw_image_data;
        }
        offset += 12 + chunk_data_len;
    }
    return 0;
}

static int skip_gif_sub_blocks(const unsigned char *data, size_t data_len, size_t *offset,
                               int *saw_data)
{
    while (*offset < data_len) {
        size_t block_len = data[(*offset)++];
        if (block_len == 0)
            return 1;
        if (block_len > data_len - *offset)
            return 0;
        if (saw_data)
            *saw_data = 1;
        *offset += block_len;
    }
    return 0;
}

static int gif_is_complete(const unsigned char *data, size_t data_len)
{
    if (data_len < 13)
        return 0;

    size_t offset = 13;
    if (data[10] & GIF_COLOR_TABLE_FLAG) {
        size_t color_table_len = (size_t)3 << ((data[10] & GIF_COLOR_TABLE_SIZE_MASK) + 1);
        if (color_table_len > data_len - offset)
            return 0;
        offset += color_table_len;
    }

    int saw_image = 0;
    while (offset < data_len) {
        unsigned char block_type = data[offset];
        if (block_type == GIF_TRAILER)
            return saw_image && offset + 1 == data_len;
        if (block_type == GIF_EXTENSION) {
            if (data_len - offset < 2)
                return 0;
            offset += 2;
            if (!skip_gif_sub_blocks(data, data_len, &offset, NULL))
                return 0;
            continue;
        }
        if (block_type != GIF_IMAGE_DESCRIPTOR || data_len - offset < 10)
            return 0;

        unsigned char flags = data[offset + 9];
        offset += 10;
        if (flags & GIF_COLOR_TABLE_FLAG) {
            size_t color_table_len = (size_t)3 << ((flags & GIF_COLOR_TABLE_SIZE_MASK) + 1);
            if (color_table_len > data_len - offset)
                return 0;
            offset += color_table_len;
        }
        if (offset >= data_len)
            return 0;
        offset++; /* LZW minimum code size */

        int saw_compressed_data = 0;
        if (!skip_gif_sub_blocks(data, data_len, &offset, &saw_compressed_data) ||
            !saw_compressed_data)
            return 0;
        saw_image = 1;
    }
    return 0;
}

static int next_webp_chunk(const unsigned char *data, size_t data_len, size_t *offset,
                           const unsigned char **chunk_type, const unsigned char **chunk_data,
                           size_t *chunk_data_len)
{
    if (*offset > data_len || data_len - *offset < 8)
        return 0;

    size_t chunk_offset = *offset;
    size_t payload_len = read_le32(data + chunk_offset + 4);
    if (payload_len > data_len - chunk_offset - 8)
        return 0;

    size_t next_offset = chunk_offset + 8 + payload_len;
    if (payload_len & 1) {
        if (next_offset >= data_len)
            return 0;
        next_offset++;
    }

    *chunk_type = data + chunk_offset;
    *chunk_data = data + chunk_offset + 8;
    *chunk_data_len = payload_len;
    *offset = next_offset;
    return 1;
}

static int webp_lossy_payload_is_valid(const unsigned char *data, size_t data_len)
{
    return data_len >= 10 && data[3] == 0x9d && data[4] == 0x01 && data[5] == 0x2a;
}

static int webp_lossless_payload_is_valid(const unsigned char *data, size_t data_len)
{
    return data_len >= 5 && data[0] == 0x2f;
}

static int webp_animation_frame_is_complete(const unsigned char *data, size_t data_len)
{
    if (data_len < 16)
        return 0;

    size_t offset = 16;
    int saw_image_data = 0;
    while (offset < data_len) {
        const unsigned char *chunk_type;
        const unsigned char *chunk_data;
        size_t chunk_data_len;
        if (!next_webp_chunk(data, data_len, &offset, &chunk_type, &chunk_data, &chunk_data_len))
            return 0;

        if (memcmp(chunk_type, "VP8 ", 4) == 0) {
            if (!webp_lossy_payload_is_valid(chunk_data, chunk_data_len))
                return 0;
            saw_image_data = 1;
        } else if (memcmp(chunk_type, "VP8L", 4) == 0) {
            if (!webp_lossless_payload_is_valid(chunk_data, chunk_data_len))
                return 0;
            saw_image_data = 1;
        }
    }
    return saw_image_data;
}

static int webp_is_complete(const unsigned char *data, size_t data_len)
{
    size_t riff_data_len = read_le32(data + 4);
    if (riff_data_len < 4 || riff_data_len > data_len - 8)
        return 0;

    size_t chunks_len = riff_data_len - 4;
    size_t offset = 0;
    int saw_image_data = 0;
    while (offset < chunks_len) {
        const unsigned char *chunk_type;
        const unsigned char *chunk_data;
        size_t chunk_data_len;
        if (!next_webp_chunk(data + 12, chunks_len, &offset, &chunk_type, &chunk_data,
                             &chunk_data_len))
            return 0;

        if (memcmp(chunk_type, "VP8 ", 4) == 0) {
            if (!webp_lossy_payload_is_valid(chunk_data, chunk_data_len))
                return 0;
            saw_image_data = 1;
        } else if (memcmp(chunk_type, "VP8L", 4) == 0) {
            if (!webp_lossless_payload_is_valid(chunk_data, chunk_data_len))
                return 0;
            saw_image_data = 1;
        } else if (memcmp(chunk_type, "ANMF", 4) == 0) {
            if (!webp_animation_frame_is_complete(chunk_data, chunk_data_len))
                return 0;
            saw_image_data = 1;
        }
    }
    return saw_image_data;
}

static void inspect_png(const unsigned char *data, size_t data_len, struct image_info *info)
{
    info->mime = "image/png";
    if (data_len >= 24 && read_be32(data + 8) == 13 && memcmp(data + 12, "IHDR", 4) == 0) {
        info->width = read_be32(data + 16);
        info->height = read_be32(data + 20);
    }
    info->complete = png_is_complete(data, data_len);
}

static void inspect_gif(const unsigned char *data, size_t data_len, struct image_info *info)
{
    info->mime = "image/gif";
    if (data_len >= 10) {
        info->width = read_le16(data + 6);
        info->height = read_le16(data + 8);
    }
    info->complete = gif_is_complete(data, data_len);
}

static void inspect_jpeg(const unsigned char *data, size_t data_len, struct image_info *info)
{
    info->mime = "image/jpeg";
    read_jpeg_dimensions(data, data_len, info);
    info->complete = jpeg_is_complete(data, data_len);
}

static void inspect_webp(const unsigned char *data, size_t data_len, struct image_info *info)
{
    info->mime = "image/webp";
    info->complete = webp_is_complete(data, data_len);

    if (data_len < 20)
        return;
    const unsigned char *chunk_type = data + 12;
    size_t chunk_data_len = read_le32(data + 16);
    const unsigned char *chunk_data = data + 20;
    size_t available_chunk_data_len = data_len - 20;

    if (memcmp(chunk_type, "VP8 ", 4) == 0 && available_chunk_data_len >= chunk_data_len &&
        webp_lossy_payload_is_valid(chunk_data, chunk_data_len)) {
        /* Lossy frames put 14-bit dimensions after the three-byte sync code. */
        info->width = read_le16(chunk_data + 6) & 0x3fff;
        info->height = read_le16(chunk_data + 8) & 0x3fff;
    } else if (memcmp(chunk_type, "VP8L", 4) == 0 && available_chunk_data_len >= chunk_data_len &&
               webp_lossless_payload_is_valid(chunk_data, chunk_data_len)) {
        /* Lossless frames pack 14-bit (dimension - 1) values. */
        uint32_t dimensions = read_le32(chunk_data + 1);
        info->width = (long)(dimensions & 0x3fff) + 1;
        info->height = (long)((dimensions >> 14) & 0x3fff) + 1;
    } else if (memcmp(chunk_type, "VP8X", 4) == 0 && chunk_data_len >= 10 &&
               available_chunk_data_len >= 10) {
        /* The extended header stores 24-bit (dimension - 1) canvas values. */
        info->width = read_le24(chunk_data + 4) + 1;
        info->height = read_le24(chunk_data + 7) + 1;
    }
}

int image_sniff(const void *buf, size_t buf_len, struct image_info *info)
{
    static const unsigned char PNG_SIGNATURE[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    const unsigned char *data = buf;

    *info = (struct image_info){0};
    if (buf_len >= sizeof(PNG_SIGNATURE) &&
        memcmp(data, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0) {
        inspect_png(data, buf_len, info);
        return 1;
    }
    if (buf_len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) {
        inspect_gif(data, buf_len, info);
        return 1;
    }
    if (buf_len >= 3 && data[0] == JPEG_MARKER_PREFIX && data[1] == JPEG_SOI &&
        data[2] == JPEG_MARKER_PREFIX) {
        inspect_jpeg(data, buf_len, info);
        return 1;
    }
    if (buf_len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        inspect_webp(data, buf_len, info);
        return 1;
    }
    return 0;
}
