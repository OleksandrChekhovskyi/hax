/* SPDX-License-Identifier: MIT */
#include "buf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void buf_init(struct buf *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void buf_free(struct buf *buf)
{
    free(buf->data);
    buf_init(buf);
}

static void buf_grow(struct buf *buf, size_t required_capacity)
{
    if (buf->cap >= required_capacity)
        return;

    size_t capacity = buf->cap ? buf->cap : 256;
    while (capacity < required_capacity) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required_capacity;
            break;
        }
        capacity *= 2;
    }
    buf->data = xrealloc(buf->data, capacity);
    buf->cap = capacity;
}

void buf_append(struct buf *buf, const void *data, size_t length)
{
    if (buf->len == SIZE_MAX || length > SIZE_MAX - buf->len - 1)
        die_oom();
    buf_grow(buf, buf->len + length + 1);
    if (length > 0)
        memcpy(buf->data + buf->len, data, length);
    buf->len += length;
    buf->data[buf->len] = '\0';
}

void buf_append_str(struct buf *buf, const char *str)
{
    buf_append(buf, str, strlen(str));
}

void buf_reset(struct buf *buf)
{
    buf->len = 0;
    if (buf->data)
        buf->data[0] = '\0';
}

char *buf_steal(struct buf *buf)
{
    char *data = buf->data ? buf->data : xstrdup("");
    buf_init(buf);
    return data;
}
