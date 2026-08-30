/* SPDX-License-Identifier: MIT */
#ifndef HAX_BUF_H
#define HAX_BUF_H

#include <stddef.h>

/* Append-only byte buffer, NUL-terminated whenever data is non-NULL. */
struct buf {
    char *data;
    size_t len;
    size_t cap;
};

void buf_init(struct buf *buf);
void buf_free(struct buf *buf);
void buf_append(struct buf *buf, const void *data, size_t length);
void buf_append_str(struct buf *buf, const char *str);
void buf_reset(struct buf *buf);
/* Transfer an allocated NUL-terminated string to the caller and reset buf. */
char *buf_steal(struct buf *buf);

#endif /* HAX_BUF_H */
