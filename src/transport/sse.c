/* SPDX-License-Identifier: MIT */
#include "transport/sse.h"

#include <string.h>

void sse_parser_init(struct sse_parser *p, sse_cb cb, void *user)
{
    memset(p, 0, sizeof(*p));
    p->cb = cb;
    p->user = user;
}

void sse_parser_free(struct sse_parser *p)
{
    buf_free(&p->line);
    buf_free(&p->event);
    buf_free(&p->data);
}

static void emit_event(struct sse_parser *p)
{
    if (p->event.len == 0 && p->data.len == 0)
        return;
    if (!p->cb_abort) {
        const char *ev = p->event.data ? p->event.data : "";
        const char *dt = p->data.data ? p->data.data : "";
        if (p->cb(ev, dt, p->user) != 0)
            p->cb_abort = 1;
    }
    buf_reset(&p->event);
    buf_reset(&p->data);
}

static void process_line(struct sse_parser *p, const char *line, size_t n)
{
    if (n && line[n - 1] == '\r')
        n--;

    if (n == 0) {
        emit_event(p);
        return;
    }
    if (line[0] == ':')
        return; /* comment */

    const char *colon = memchr(line, ':', n);
    const char *field = line;
    size_t field_len = colon ? (size_t)(colon - line) : n;
    const char *value = colon ? colon + 1 : "";
    size_t value_len = colon ? n - field_len - 1 : 0;
    if (value_len && value[0] == ' ') {
        value++;
        value_len--;
    }

    if (field_len == 5 && memcmp(field, "event", 5) == 0) {
        buf_reset(&p->event);
        buf_append(&p->event, value, value_len);
    } else if (field_len == 4 && memcmp(field, "data", 4) == 0) {
        if (p->data.len > 0)
            buf_append(&p->data, "\n", 1);
        buf_append(&p->data, value, value_len);
    }
    /* ignore id:, retry:, and unknown fields */
}

void sse_parser_feed(struct sse_parser *p, const char *data, size_t n)
{
    size_t i = 0;
    for (size_t j = 0; j < n; j++) {
        if (data[j] == '\n') {
            buf_append(&p->line, data + i, j - i);
            process_line(p, p->line.data ? p->line.data : "", p->line.len);
            buf_reset(&p->line);
            i = j + 1;
        }
    }
    if (i < n)
        buf_append(&p->line, data + i, n - i);
}

void sse_parser_finalize(struct sse_parser *p)
{
    if (p->line.len)
        process_line(p, p->line.data, p->line.len);
    if (p->event.len || p->data.len)
        emit_event(p);
}
