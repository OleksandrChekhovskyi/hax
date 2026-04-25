/* SPDX-License-Identifier: MIT */
#ifndef HAX_UTIL_H
#define HAX_UTIL_H

#include <stddef.h>

/* Allocate-or-die helpers. On OOM they print to stderr and abort — we are a
 * CLI, not a library; a clean crash is better than leaking partial state. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Read an entire file into a newly-allocated NUL-terminated string.
 * Returns NULL on error and sets errno. Caller frees. */
char *slurp_file(const char *path, size_t *out_len);

/* Write exactly n bytes to fd, restarting on EINTR/short writes. Returns 0
 * on success, -1 on error (errno set). */
int write_all(int fd, const void *data, size_t n);

/* Read up to cap bytes from path into a newly-allocated NUL-terminated
 * string. Allocates at most cap+1 bytes regardless of file size. If the
 * file has more bytes than the cap, sets *out_truncated to 1. Returns
 * NULL on error and sets errno. Caller frees. */
char *slurp_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated);

/* Expand a leading ~ to $HOME. Returns a newly-allocated path. */
char *expand_home(const char *path);

/* Write a random UUIDv4 (36 chars + NUL terminator) to out. Aborts on
 * failure (e.g. /dev/urandom unavailable) — same convention as xmalloc. */
void gen_uuid_v4(char out[37]);

/* Convert an arbitrary byte range into a NUL-free, valid UTF-8 string.
 * Valid UTF-8 is preserved verbatim; NULs and invalid sequences are
 * replaced with U+FFFD. Caller frees. */
char *sanitize_utf8(const char *data, size_t len);

/* Dynamic byte buffer — append-only, NUL-terminated at current length. */
struct buf {
    char *data;
    size_t len;
    size_t cap;
};

void buf_init(struct buf *b);
void buf_free(struct buf *b);
void buf_append(struct buf *b, const void *data, size_t n);
void buf_append_str(struct buf *b, const char *s);
void buf_reset(struct buf *b);
char *buf_steal(struct buf *b); /* takes ownership, re-inits buf */

#endif /* HAX_UTIL_H */
