/* SPDX-License-Identifier: MIT */
#include "text/utf8.h"

#include <stdint.h>
#include <wchar.h>

static int byte_is_continuation(unsigned char byte)
{
    return (byte & 0xC0) == 0x80;
}

size_t utf8_sequence_length(unsigned char byte)
{
    if (byte < 0x80)
        return 1;
    if (byte >= 0xC2 && byte <= 0xDF)
        return 2;
    if (byte >= 0xE0 && byte <= 0xEF)
        return 3;
    if (byte >= 0xF0 && byte <= 0xF4)
        return 4;
    return 1;
}

int utf8_sequence_is_valid(const char *bytes, size_t length)
{
    const unsigned char *input = (const unsigned char *)bytes;
    if (length < 1 || length > 4 || utf8_sequence_length(input[0]) != length)
        return 0;
    if (length == 1)
        return input[0] < 0x80;

    uint32_t codepoint;
    if (length == 2)
        codepoint = input[0] & 0x1F;
    else if (length == 3)
        codepoint = input[0] & 0x0F;
    else
        codepoint = input[0] & 0x07;

    for (size_t i = 1; i < length; i++) {
        if (!byte_is_continuation(input[i]))
            return 0;
        codepoint = (codepoint << 6) | (input[i] & 0x3F);
    }

    static const uint32_t minimum_codepoint[] = {0, 0, 0x80, 0x800, 0x10000};
    if (codepoint < minimum_codepoint[length])
        return 0;
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
        return 0;
    return codepoint <= 0x10FFFF;
}

int utf8_is_valid(const char *bytes, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        size_t sequence_len = utf8_sequence_length((unsigned char)bytes[offset]);
        if (sequence_len > length - offset || !utf8_sequence_is_valid(bytes + offset, sequence_len))
            return 0;
        offset += sequence_len;
    }
    return 1;
}

size_t utf8_next(const char *bytes, size_t length, size_t offset)
{
    if (offset >= length)
        return length;

    size_t sequence_len = utf8_sequence_length((unsigned char)bytes[offset]);
    if (sequence_len > length - offset || !utf8_sequence_is_valid(bytes + offset, sequence_len))
        return offset + 1;
    return offset + sequence_len;
}

size_t utf8_prev(const char *bytes, size_t offset)
{
    if (offset == 0)
        return 0;

    size_t candidate = offset - 1;
    size_t continuation_count = 0;
    while (candidate > 0 && byte_is_continuation((unsigned char)bytes[candidate]) &&
           continuation_count < 3) {
        candidate--;
        continuation_count++;
    }

    unsigned char leader = (unsigned char)bytes[candidate];
    if (byte_is_continuation(leader))
        return offset - 1;
    if (continuation_count == 0)
        return candidate;

    size_t sequence_len = continuation_count + 1;
    if (utf8_sequence_length(leader) != sequence_len ||
        !utf8_sequence_is_valid(bytes + candidate, sequence_len))
        return offset - 1;
    return candidate;
}

/* Terminal format controls can hide or reorder content even when libc assigns them zero width.
 * Variation selectors remain available for emoji presentation, though substituting ZWJ can break
 * joined emoji sequences. */
static int codepoint_requires_substitution(wchar_t codepoint)
{
    if (codepoint == 0x00AD) /* soft hyphen */
        return 1;
    if (codepoint == 0x034F) /* combining grapheme joiner */
        return 1;
    if (codepoint == 0x061C) /* Arabic letter mark */
        return 1;
    if (codepoint == 0x115F || codepoint == 0x1160) /* Hangul fillers */
        return 1;
    if (codepoint == 0x180E) /* Mongolian vowel separator */
        return 1;
    if (codepoint >= 0x200B && codepoint <= 0x200F) /* ZWSP, ZWNJ, ZWJ, LRM, RLM */
        return 1;
    if (codepoint >= 0x2028 && codepoint <= 0x2029) /* line and paragraph separators */
        return 1;
    if (codepoint >= 0x202A && codepoint <= 0x202E) /* bidi overrides */
        return 1;
    if (codepoint >= 0x2060 && codepoint <= 0x206F) /* joiners, isolates, deprecated controls */
        return 1;
    if (codepoint == 0x3164) /* Hangul filler */
        return 1;
    if (codepoint == 0xFEFF) /* BOM / ZWNBSP */
        return 1;
    if (codepoint == 0xFFA0) /* halfwidth Hangul filler */
        return 1;
    if (codepoint >= 0xFFF9 && codepoint <= 0xFFFB) /* interlinear annotation marks */
        return 1;
    if (codepoint >= 0xE0000 && codepoint <= 0xE007F) /* language tags */
        return 1;
    return 0;
}

int utf8_codepoint_cells(const char *bytes, size_t length, size_t offset, size_t *codepoint_len)
{
    if (offset >= length) {
        *codepoint_len = 0;
        return 0;
    }

    wchar_t codepoint;
    mbstate_t state = {0};
    size_t decoded_len = mbrtowc(&codepoint, bytes + offset, length - offset, &state);
    if (decoded_len == (size_t)-1 || decoded_len == (size_t)-2 || decoded_len == 0) {
        *codepoint_len = 1;
        return -1;
    }

    *codepoint_len = decoded_len;
    if (codepoint_requires_substitution(codepoint))
        return -1;
    return wcwidth(codepoint);
}

void utf8_cell_stream_reset(struct utf8_cell_stream *stream)
{
    stream->pending_len = 0;
}

static int pending_display_cells(const struct utf8_cell_stream *stream)
{
    size_t codepoint_len;
    int cells =
        utf8_codepoint_cells((const char *)stream->pending, stream->pending_len, 0, &codepoint_len);
    return cells < 0 ? 1 : cells;
}

int utf8_cell_stream_feed(struct utf8_cell_stream *stream, unsigned char byte, const char **bytes,
                          size_t *length, int *cells)
{
    if (stream->pending_len == 0) {
        stream->pending[0] = byte;
        stream->pending_len = 1;
        if (utf8_sequence_length(byte) > 1)
            return 0;

        *bytes = (const char *)stream->pending;
        *length = 1;
        *cells = pending_display_cells(stream);
        stream->pending_len = 0;
        return 1;
    }

    if (!byte_is_continuation(byte)) {
        stream->pending[stream->pending_len++] = byte;
        *bytes = (const char *)stream->pending;
        *length = stream->pending_len;
        *cells = (int)stream->pending_len;
        stream->pending_len = 0;
        return 1;
    }

    stream->pending[stream->pending_len++] = byte;
    size_t sequence_len = utf8_sequence_length(stream->pending[0]);
    if (stream->pending_len < sequence_len)
        return 0;

    *bytes = (const char *)stream->pending;
    *length = stream->pending_len;
    *cells = utf8_sequence_is_valid((const char *)stream->pending, sequence_len)
                 ? pending_display_cells(stream)
                 : (int)stream->pending_len;
    stream->pending_len = 0;
    return 1;
}

int utf8_cell_stream_flush(struct utf8_cell_stream *stream, const char **bytes, size_t *length,
                           int *cells)
{
    if (stream->pending_len == 0)
        return 0;

    *bytes = (const char *)stream->pending;
    *length = stream->pending_len;
    *cells = (int)stream->pending_len;
    stream->pending_len = 0;
    return 1;
}
