/* SPDX-License-Identifier: MIT */
#include "render/ctrl_strip.h"

#include <string.h>

#include "xalloc.h"

enum {
    ASCII_BEL = 0x07,
    ASCII_CAN = 0x18,
    ASCII_SUB = 0x1a,
    ASCII_ESC = 0x1b,
    ASCII_DEL = 0x7f,
};

static int is_text_byte(unsigned char byte)
{
    return byte == '\t' || byte == '\n' || (byte >= 0x20 && byte != ASCII_DEL);
}

static int cancels_sequence(unsigned char byte)
{
    /* CAN and SUB are ECMA-48 cancellation bytes. LF also bounds malformed sequences in streamed,
     * line-oriented output. */
    return byte == '\n' || byte == ASCII_CAN || byte == ASCII_SUB;
}

static int is_escape_intermediate(unsigned char byte)
{
    return byte >= 0x20 && byte <= 0x2f;
}

static int is_escape_final(unsigned char byte)
{
    return byte >= 0x30 && byte <= 0x7e;
}

static int is_csi_final(unsigned char byte)
{
    return byte >= 0x40 && byte <= 0x7e;
}

enum byte_action {
    BYTE_CONSUME,
    BYTE_EMIT,
    BYTE_REPROCESS,
};

static enum byte_action ctrl_strip_step(struct ctrl_strip *strip, unsigned char byte)
{
    switch (strip->state) {
    case CTRL_STRIP_TEXT:
        if (byte == ASCII_ESC)
            strip->state = CTRL_STRIP_ESCAPE;
        else if (is_text_byte(byte))
            return BYTE_EMIT;
        return BYTE_CONSUME;
    case CTRL_STRIP_ESCAPE:
        if (byte == '[') {
            strip->state = CTRL_STRIP_CSI;
        } else if (byte == ']') {
            strip->state = CTRL_STRIP_OSC;
        } else if (byte == 'P' || byte == '^' || byte == '_') {
            strip->state = CTRL_STRIP_CONTROL_STRING;
        } else if (is_escape_intermediate(byte)) {
            strip->state = CTRL_STRIP_ESCAPE_INTERMEDIATE;
        } else if (is_escape_final(byte)) {
            strip->state = CTRL_STRIP_TEXT;
        } else {
            strip->state = CTRL_STRIP_TEXT;
            return BYTE_REPROCESS;
        }
        return BYTE_CONSUME;
    case CTRL_STRIP_CSI:
        if (cancels_sequence(byte)) {
            strip->state = CTRL_STRIP_TEXT;
            return BYTE_REPROCESS;
        }
        if (is_csi_final(byte))
            strip->state = CTRL_STRIP_TEXT;
        return BYTE_CONSUME;
    case CTRL_STRIP_OSC:
        if (cancels_sequence(byte)) {
            strip->state = CTRL_STRIP_TEXT;
            return BYTE_REPROCESS;
        }
        if (byte == ASCII_BEL)
            strip->state = CTRL_STRIP_TEXT;
        else if (byte == ASCII_ESC)
            strip->state = CTRL_STRIP_OSC_ESCAPE;
        return BYTE_CONSUME;
    case CTRL_STRIP_OSC_ESCAPE:
        if (byte == '\\') {
            strip->state = CTRL_STRIP_TEXT;
            return BYTE_CONSUME;
        }
        if (cancels_sequence(byte))
            strip->state = CTRL_STRIP_TEXT;
        else
            strip->state = CTRL_STRIP_OSC;
        return BYTE_REPROCESS;
    case CTRL_STRIP_CONTROL_STRING:
        if (cancels_sequence(byte)) {
            strip->state = CTRL_STRIP_TEXT;
            return BYTE_REPROCESS;
        }
        if (byte == ASCII_ESC)
            strip->state = CTRL_STRIP_CONTROL_STRING_ESCAPE;
        return BYTE_CONSUME;
    case CTRL_STRIP_CONTROL_STRING_ESCAPE:
        if (byte == '\\') {
            strip->state = CTRL_STRIP_TEXT;
            return BYTE_CONSUME;
        }
        if (cancels_sequence(byte))
            strip->state = CTRL_STRIP_TEXT;
        else
            strip->state = CTRL_STRIP_CONTROL_STRING;
        return BYTE_REPROCESS;
    case CTRL_STRIP_ESCAPE_INTERMEDIATE:
        if (cancels_sequence(byte)) {
            strip->state = CTRL_STRIP_TEXT;
            return BYTE_REPROCESS;
        }
        if (is_escape_final(byte))
            strip->state = CTRL_STRIP_TEXT;
        return BYTE_CONSUME;
    }

    return BYTE_CONSUME;
}

void ctrl_strip_init(struct ctrl_strip *strip)
{
    strip->state = CTRL_STRIP_TEXT;
}

size_t ctrl_strip_feed(struct ctrl_strip *strip, const char *input, size_t input_len, char *output)
{
    size_t output_len = 0;

    for (size_t input_offset = 0; input_offset < input_len; input_offset++) {
        unsigned char byte = (unsigned char)input[input_offset];
        enum byte_action action;

        do {
            action = ctrl_strip_step(strip, byte);
        } while (action == BYTE_REPROCESS);
        if (action == BYTE_EMIT)
            output[output_len++] = (char)byte;
    }

    return output_len;
}

char *ctrl_strip_dup(const char *input)
{
    size_t input_len = strlen(input);
    char *output = xmalloc(input_len + 1);
    struct ctrl_strip strip;

    ctrl_strip_init(&strip);
    size_t output_len = ctrl_strip_feed(&strip, input, input_len, output);
    output[output_len] = '\0';
    return output;
}

char *ctrl_strip_line_dup(const char *input)
{
    char *output = ctrl_strip_dup(input);
    for (char *c = output; *c; c++)
        if (*c == '\n' || *c == '\t')
            *c = ' ';
    return output;
}
