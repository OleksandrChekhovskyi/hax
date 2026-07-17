/* SPDX-License-Identifier: MIT */
#include "render/markdown_scan.h"

static struct md_line_info line_info(enum md_line_kind kind, size_t indent_length)
{
    return (struct md_line_info){
        .kind = kind,
        .indent_length = indent_length,
        .classification_complete = 1,
    };
}

static struct md_line_info incomplete_line(size_t indent_length)
{
    struct md_line_info info = line_info(MD_LINE_INCOMPLETE, indent_length);
    info.classification_complete = 0;
    return info;
}

struct md_line_info md_scan_line(const char *line, size_t length, int final)
{
    size_t marker_offset = 0;
    int has_tab = 0;
    while (marker_offset < length && (line[marker_offset] == ' ' || line[marker_offset] == '\t')) {
        if (line[marker_offset] == '\t')
            has_tab = 1;
        marker_offset++;
    }

    if (marker_offset >= length) {
        if (!final)
            return incomplete_line(marker_offset);
        return line_info(MD_LINE_TEXT, marker_offset);
    }

    if (line[marker_offset] == '\n') {
        struct md_line_info info = line_info(MD_LINE_BLANK, marker_offset);
        info.normalize_indent = 1;
        return info;
    }

    int block_indent = !has_tab && marker_offset <= 3;
    char marker = line[marker_offset];

    if (block_indent && marker == '#') {
        size_t end = marker_offset;
        while (end < length && end - marker_offset < 6 && line[end] == '#')
            end++;
        if (end >= length)
            return final ? line_info(MD_LINE_TEXT, marker_offset) : incomplete_line(marker_offset);
        if (line[end] == ' ' || line[end] == '\n') {
            struct md_line_info info = line_info(MD_LINE_HEADING, marker_offset);
            info.marker = marker;
            info.marker_length = end - marker_offset;
            info.normalize_indent = 1;
            return info;
        }
        return line_info(MD_LINE_TEXT, marker_offset);
    }

    if (block_indent && marker == '`') {
        size_t available = length - marker_offset;
        if (available < 3)
            return final ? line_info(MD_LINE_TEXT, marker_offset) : incomplete_line(marker_offset);
        if (line[marker_offset + 1] == '`' && line[marker_offset + 2] == '`') {
            size_t end = marker_offset + 3;
            while (end < length && line[end] == '`')
                end++;
            struct md_line_info info = line_info(MD_LINE_FENCE, marker_offset);
            info.marker = marker;
            info.marker_length = end - marker_offset;
            info.normalize_indent = 1;
            while (end < length && line[end] != '\n')
                end++;
            if (end >= length) {
                if (final)
                    return line_info(MD_LINE_TEXT, marker_offset);
                info.classification_complete = 0;
            }
            return info;
        }
        return line_info(MD_LINE_TEXT, marker_offset);
    }

    int bullet = 0;
    if (!has_tab && (marker == '*' || marker == '-' || marker == '+')) {
        if (marker_offset + 1 >= length) {
            if (!final)
                return incomplete_line(marker_offset);
        } else if (line[marker_offset + 1] == ' ') {
            bullet = 1;
        }
    }

    if (block_indent && (marker == '-' || marker == '*' || marker == '_' || marker == '=')) {
        size_t end = marker_offset;
        size_t count = 0;
        while (end < length && (line[end] == marker || line[end] == ' ' || line[end] == '\t' ||
                                line[end] == '\r')) {
            if (line[end] == marker)
                count++;
            end++;
        }
        if (end >= length) {
            if (final && count >= 3) {
                struct md_line_info info = line_info(MD_LINE_THEMATIC, marker_offset);
                info.marker = marker;
                info.marker_length = count;
                info.normalize_indent = 1;
                return info;
            }
            if (!final && bullet) {
                struct md_line_info info = line_info(MD_LINE_BULLET, marker_offset);
                info.marker = marker;
                info.marker_length = 1;
                info.classification_complete = 0;
                return info;
            }
            return final ? line_info(MD_LINE_TEXT, marker_offset) : incomplete_line(marker_offset);
        }
        if (line[end] == '\n' && count >= 3) {
            struct md_line_info info = line_info(MD_LINE_THEMATIC, marker_offset);
            info.marker = marker;
            info.marker_length = count;
            info.normalize_indent = 1;
            return info;
        }
    }

    if (bullet) {
        struct md_line_info info = line_info(MD_LINE_BULLET, marker_offset);
        info.marker = marker;
        info.marker_length = 1;
        return info;
    }

    if (!has_tab && marker >= '0' && marker <= '9') {
        size_t end = marker_offset;
        while (end < length && line[end] >= '0' && line[end] <= '9')
            end++;
        if (end >= length || end + 1 >= length)
            return final ? line_info(MD_LINE_TEXT, marker_offset) : incomplete_line(marker_offset);
        if ((line[end] == '.' || line[end] == ')') && line[end + 1] == ' ') {
            struct md_line_info info = line_info(MD_LINE_ORDERED, marker_offset);
            info.marker = line[end];
            info.marker_length = end - marker_offset + 1;
            return info;
        }
    }

    if (block_indent && marker == '>') {
        struct md_line_info info = line_info(MD_LINE_BLOCKQUOTE, marker_offset);
        info.marker = marker;
        info.marker_length = 1;
        info.normalize_indent = 1;
        return info;
    }

    if (block_indent && marker == '|') {
        struct md_line_info info = line_info(MD_LINE_PIPE, marker_offset);
        info.marker = marker;
        info.marker_length = 1;
        info.normalize_indent = 1;
        return info;
    }

    return line_info(MD_LINE_TEXT, marker_offset);
}
