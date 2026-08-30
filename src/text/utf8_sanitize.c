/* SPDX-License-Identifier: MIT */
#include "text/utf8_sanitize.h"

#include <string.h>

#include "buf.h"
#include "util.h"
#include "text/utf8.h"

static const char REPLACEMENT[] = "\xEF\xBF\xBD";

static size_t emit_replacements(char *output, size_t count)
{
    for (size_t i = 0; i < count; i++)
        memcpy(output + i * 3, REPLACEMENT, 3);
    return count * 3;
}

void utf8_sanitizer_init(struct utf8_sanitizer *sanitizer)
{
    sanitizer->pending_len = 0;
}

static int byte_is_continuation(unsigned char byte)
{
    return (byte & 0xC0) == 0x80;
}

size_t utf8_sanitizer_feed(struct utf8_sanitizer *sanitizer, const char *input, size_t input_len,
                           char *output)
{
    size_t output_len = 0;
    for (size_t i = 0; i < input_len; i++) {
        unsigned char byte = (unsigned char)input[i];

        if (sanitizer->pending_len > 0) {
            if (byte_is_continuation(byte)) {
                sanitizer->pending[sanitizer->pending_len++] = byte;
                size_t sequence_len = utf8_sequence_length(sanitizer->pending[0]);
                if (sanitizer->pending_len < sequence_len)
                    continue;

                if (utf8_sequence_is_valid((const char *)sanitizer->pending, sequence_len)) {
                    memcpy(output + output_len, sanitizer->pending, sequence_len);
                    output_len += sequence_len;
                } else {
                    output_len += emit_replacements(output + output_len, sanitizer->pending_len);
                }
                sanitizer->pending_len = 0;
                continue;
            }

            output_len += emit_replacements(output + output_len, sanitizer->pending_len);
            sanitizer->pending_len = 0;
        }

        if (byte == 0) {
            output_len += emit_replacements(output + output_len, 1);
            continue;
        }
        if (byte < 0x80) {
            output[output_len++] = (char)byte;
            continue;
        }

        size_t sequence_len = utf8_sequence_length(byte);
        if (sequence_len == 1) {
            output_len += emit_replacements(output + output_len, 1);
            continue;
        }

        sanitizer->pending[0] = byte;
        sanitizer->pending_len = 1;
    }
    return output_len;
}

size_t utf8_sanitizer_flush(struct utf8_sanitizer *sanitizer, char *output)
{
    size_t output_len = emit_replacements(output, sanitizer->pending_len);
    sanitizer->pending_len = 0;
    return output_len;
}

char *utf8_sanitize(const char *input, size_t input_len)
{
    /* Bounded scratch avoids reserving three times the full input for mostly valid text. */
    enum { INPUT_CHUNK_SIZE = (4096 - 9) / 3 };
    char scratch[UTF8_SANITIZE_FEED_MAX(INPUT_CHUNK_SIZE)];
    struct utf8_sanitizer sanitizer;
    utf8_sanitizer_init(&sanitizer);

    struct buf output;
    buf_init(&output);
    for (size_t offset = 0; offset < input_len;) {
        size_t chunk_len = input_len - offset;
        if (chunk_len > INPUT_CHUNK_SIZE)
            chunk_len = INPUT_CHUNK_SIZE;
        size_t sanitized_len = utf8_sanitizer_feed(&sanitizer, input + offset, chunk_len, scratch);
        buf_append(&output, scratch, sanitized_len);
        offset += chunk_len;
    }

    size_t tail_len = utf8_sanitizer_flush(&sanitizer, scratch);
    buf_append(&output, scratch, tail_len);
    if (!output.data)
        return xstrdup("");
    return buf_steal(&output);
}
