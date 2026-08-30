/* SPDX-License-Identifier: MIT */
#include "transport/sse.h"

#include <string.h>

#include "buf.h"

void sse_parser_init(struct sse_parser *parser, sse_cb callback, void *user)
{
    memset(parser, 0, sizeof(*parser));
    parser->callback = callback;
    parser->callback_user = user;
}

void sse_parser_free(struct sse_parser *parser)
{
    buf_free(&parser->line);
    buf_free(&parser->event);
    buf_free(&parser->data);
}

static void emit_event(struct sse_parser *parser)
{
    if (parser->event.len == 0 && parser->data.len == 0)
        return;

    if (!parser->callback_stopped) {
        const char *event_name = parser->event.data ? parser->event.data : "";
        const char *data = parser->data.data ? parser->data.data : "";
        if (parser->callback(event_name, data, parser->callback_user) != 0)
            parser->callback_stopped = 1;
    }
    buf_reset(&parser->event);
    buf_reset(&parser->data);
}

static void process_line(struct sse_parser *parser, const char *line, size_t len)
{
    if (len > 0 && line[len - 1] == '\r')
        len--;

    if (len == 0) {
        emit_event(parser);
        return;
    }
    if (line[0] == ':')
        return;

    const char *colon = memchr(line, ':', len);
    size_t field_len = colon ? (size_t)(colon - line) : len;
    const char *value = colon ? colon + 1 : "";
    size_t value_len = colon ? len - field_len - 1 : 0;
    if (value_len > 0 && value[0] == ' ') {
        value++;
        value_len--;
    }

    if (field_len == 5 && memcmp(line, "event", 5) == 0) {
        buf_reset(&parser->event);
        buf_append(&parser->event, value, value_len);
    } else if (field_len == 4 && memcmp(line, "data", 4) == 0) {
        if (parser->data.len > 0)
            buf_append(&parser->data, "\n", 1);
        buf_append(&parser->data, value, value_len);
    }
}

void sse_parser_feed(struct sse_parser *parser, const char *data, size_t len)
{
    size_t line_start = 0;

    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n')
            continue;

        buf_append(&parser->line, data + line_start, i - line_start);
        process_line(parser, parser->line.data ? parser->line.data : "", parser->line.len);
        buf_reset(&parser->line);
        line_start = i + 1;
    }
    if (line_start < len)
        buf_append(&parser->line, data + line_start, len - line_start);
}

void sse_parser_finalize(struct sse_parser *parser)
{
    if (parser->line.len > 0)
        process_line(parser, parser->line.data, parser->line.len);
    if (parser->event.len > 0 || parser->data.len > 0)
        emit_event(parser);
}
