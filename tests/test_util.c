/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "harness.h"

/* Helper: create a temp file with the given contents and return its path.
 * The caller owns the returned string and is responsible for unlink + free. */
static char *write_tmp(const void *data, size_t len)
{
    char *path = xstrdup("/tmp/hax-test-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        FAIL("mkstemp: %s", strerror(errno));
        free(path);
        return NULL;
    }
    if (len && write(fd, data, len) != (ssize_t)len)
        FAIL("short write to %s", path);
    close(fd);
    return path;
}

/* ---------- gen_uuid_v4 ---------- */

static int is_lower_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static void test_uuid_v4_format(void)
{
    char a[37];
    gen_uuid_v4(a);
    EXPECT(strlen(a) == 36);
    EXPECT(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
    EXPECT(a[14] == '4');
    EXPECT(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b');
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            continue;
        if (!is_lower_hex(a[i])) {
            FAIL("non-hex byte 0x%02x at position %d in %s", a[i], i, a);
            break;
        }
    }
}

static void test_uuid_v4_unique(void)
{
    char a[37], b[37];
    gen_uuid_v4(a);
    gen_uuid_v4(b);
    EXPECT(strcmp(a, b) != 0);
}

/* ---------- buf_* ---------- */

static void test_buf_append_and_steal(void)
{
    struct buf b;
    buf_init(&b);
    buf_append_str(&b, "abc");
    buf_append_str(&b, "def");
    EXPECT(b.len == 6);
    EXPECT(b.data[b.len] == '\0');
    char *s = buf_steal(&b);
    EXPECT_STR_EQ(s, "abcdef");
    EXPECT(b.data == NULL && b.len == 0 && b.cap == 0);
    free(s);
}

static void test_buf_reset_keeps_capacity(void)
{
    struct buf b;
    buf_init(&b);
    buf_append_str(&b, "hello");
    size_t cap_before = b.cap;
    buf_reset(&b);
    EXPECT(b.len == 0);
    EXPECT(b.data != NULL && b.data[0] == '\0');
    EXPECT(b.cap == cap_before);
    buf_free(&b);
}

static void test_buf_growth_crosses_default_cap(void)
{
    /* Default cap is 256; append enough to force several doublings. */
    struct buf b;
    buf_init(&b);
    char chunk[128];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 10; i++)
        buf_append(&b, chunk, sizeof(chunk));
    EXPECT(b.len == 1280);
    EXPECT(b.cap >= b.len + 1);
    EXPECT(b.data[b.len] == '\0');
    for (size_t i = 0; i < b.len; i++) {
        if (b.data[i] != 'x') {
            FAIL("corruption at offset %zu", i);
            break;
        }
    }
    buf_free(&b);
}

/* ---------- expand_home ---------- */

static void test_expand_home_null(void)
{
    EXPECT(expand_home(NULL) == NULL);
}

static void test_expand_home_no_tilde(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *p = expand_home("/absolute/path");
    EXPECT_STR_EQ(p, "/absolute/path");
    free(p);
}

static void test_expand_home_tilde_only(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *p = expand_home("~");
    EXPECT_STR_EQ(p, "/tmp/fake");
    free(p);
}

static void test_expand_home_tilde_slash(void)
{
    setenv("HOME", "/tmp/fake", 1);
    char *p = expand_home("~/sub/file");
    EXPECT_STR_EQ(p, "/tmp/fake/sub/file");
    free(p);
}

static void test_expand_home_no_home_env(void)
{
    unsetenv("HOME");
    char *p = expand_home("~/foo");
    EXPECT_STR_EQ(p, "~/foo");
    free(p);
}

/* ---------- slurp_file ---------- */

static void test_slurp_missing(void)
{
    size_t n = 999;
    char *p = slurp_file("/nonexistent/path/should-not-exist", &n);
    EXPECT(p == NULL);
}

static void test_slurp_empty(void)
{
    char *path = write_tmp("", 0);
    size_t n = 999;
    char *p = slurp_file(path, &n);
    EXPECT(p != NULL);
    EXPECT(n == 0);
    EXPECT_STR_EQ(p, "");
    free(p);
    unlink(path);
    free(path);
}

static void test_slurp_normal(void)
{
    const char content[] = "line one\nline two\n";
    size_t clen = sizeof(content) - 1;
    char *path = write_tmp(content, clen);
    size_t n = 0;
    char *p = slurp_file(path, &n);
    EXPECT(p != NULL);
    EXPECT(n == clen);
    EXPECT_MEM_EQ(p, n, content, clen);
    free(p);
    unlink(path);
    free(path);
}

static void test_slurp_directory_rejected(void)
{
    /* Some platforms let open(O_RDONLY) on a directory succeed and only
     * fail on read(); the regular-file pre-check rejects up front so
     * callers never get a bogus partial buffer back. */
    char dir[] = "/tmp/hax-test-dir-XXXXXX";
    EXPECT(mkdtemp(dir) != NULL);
    errno = 0;
    char *p = slurp_file(dir, NULL);
    EXPECT(p == NULL);
    EXPECT(errno == EISDIR);
    rmdir(dir);
}

static void test_slurp_fifo_rejected_no_hang(void)
{
    /* If this test ever hangs, the regular-file guard regressed:
     * open(O_RDONLY) on a writer-less FIFO blocks indefinitely. */
    char path[] = "/tmp/hax-test-fifo-XXXXXX";
    EXPECT(mkdtemp(path) != NULL);
    /* mkdtemp gives us a unique dir; place the FIFO inside. */
    char fifo[64];
    snprintf(fifo, sizeof(fifo), "%s/f", path);
    EXPECT(mkfifo(fifo, 0644) == 0);
    errno = 0;
    char *p = slurp_file(fifo, NULL);
    EXPECT(p == NULL);
    EXPECT(errno == EINVAL);
    /* Same check via the capped variant. */
    errno = 0;
    int truncated = 1;
    char *p2 = slurp_file_capped(fifo, 1024, NULL, &truncated);
    EXPECT(p2 == NULL);
    EXPECT(errno == EINVAL);
    unlink(fifo);
    rmdir(path);
}

/* ---------- slurp_file_capped ---------- */

static void test_slurp_capped_missing(void)
{
    size_t n = 0;
    int tr = 0;
    char *p = slurp_file_capped("/nonexistent/path/should-not-exist", 1024, &n, &tr);
    EXPECT(p == NULL);
}

static void test_slurp_capped_under(void)
{
    const char content[] = "short";
    size_t clen = sizeof(content) - 1;
    char *path = write_tmp(content, clen);
    size_t n = 0;
    int tr = 1;
    char *p = slurp_file_capped(path, 1024, &n, &tr);
    EXPECT(p != NULL);
    EXPECT(n == clen);
    EXPECT(tr == 0);
    EXPECT_STR_EQ(p, content);
    free(p);
    unlink(path);
    free(path);
}

static void test_slurp_capped_over(void)
{
    /* File is cap+100 bytes; we expect cap bytes kept and truncated=1. */
    const size_t cap = 64;
    char big[200];
    memset(big, 'a', sizeof(big));
    char *path = write_tmp(big, sizeof(big));
    size_t n = 0;
    int tr = 0;
    char *p = slurp_file_capped(path, cap, &n, &tr);
    EXPECT(p != NULL);
    EXPECT(n == cap);
    EXPECT(tr == 1);
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 'a') {
            FAIL("unexpected byte at %zu", i);
            break;
        }
    }
    EXPECT(p[n] == '\0');
    free(p);
    unlink(path);
    free(path);
}

static void test_slurp_capped_exact(void)
{
    /* File is exactly cap bytes; probe read should see EOF → truncated=0. */
    const size_t cap = 32;
    char buf[32];
    memset(buf, 'z', cap);
    char *path = write_tmp(buf, cap);
    size_t n = 0;
    int tr = 1;
    char *p = slurp_file_capped(path, cap, &n, &tr);
    EXPECT(p != NULL);
    EXPECT(n == cap);
    EXPECT(tr == 0);
    free(p);
    unlink(path);
    free(path);
}

/* ---------- cap_line_lengths ---------- */

static void test_cap_lines_no_long_lines(void)
{
    /* Short lines fall through unchanged. */
    const char in[] = "hello\nworld\n";
    size_t n = 0;
    char *out = cap_line_lengths(in, sizeof(in) - 1, 100, &n);
    EXPECT(n == sizeof(in) - 1);
    EXPECT_STR_EQ(out, in);
    free(out);
}

static void test_cap_lines_empty(void)
{
    size_t n = 99;
    char *out = cap_line_lengths("", 0, 100, &n);
    EXPECT(out != NULL);
    EXPECT(n == 0);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_cap_lines_truncates_long_line(void)
{
    /* 10 'x' bytes, capped at 4: keep first 4 + marker, then trailing nl. */
    size_t n = 0;
    char *out = cap_line_lengths("xxxxxxxxxx\n", 11, 4, &n);
    EXPECT(strstr(out, "xxxx") == out); /* starts with 4 x's */
    EXPECT(strstr(out, "[6 bytes elided]") != NULL);
    EXPECT(out[n - 1] == '\n');
    free(out);
}

static void test_cap_lines_preserves_short_neighbors(void)
{
    /* Long line in the middle gets truncated; surrounding short lines
     * are left intact and the line count is preserved. */
    char in[3100];
    int written = snprintf(in, sizeof(in), "before\n");
    memset(in + written, 'y', 2500);
    written += 2500;
    in[written++] = '\n';
    written += snprintf(in + written, sizeof(in) - written, "after\n");
    size_t n = 0;
    char *out = cap_line_lengths(in, written, 1000, &n);
    EXPECT(strstr(out, "before\n") == out);
    EXPECT(strstr(out, "\nafter\n") != NULL);
    EXPECT(strstr(out, "[1500 bytes elided]") != NULL);
    /* Three lines in, three out. */
    int nl = 0;
    for (size_t i = 0; i < n; i++)
        if (out[i] == '\n')
            nl++;
    EXPECT(nl == 3);
    free(out);
}

static void test_cap_lines_long_line_no_trailing_newline(void)
{
    /* Last "line" has no terminator; cap still applies and the result
     * has no extra newline appended. */
    size_t n = 0;
    char *out = cap_line_lengths("zzzzzzzz", 8, 3, &n);
    EXPECT(strstr(out, "zzz") == out);
    EXPECT(strstr(out, "[5 bytes elided]") != NULL);
    EXPECT(out[n - 1] != '\n');
    free(out);
}

/* ---------- parse_duration_ms ---------- */

static void test_parse_duration_plain_seconds(void)
{
    /* No suffix: number is interpreted as seconds, returned as ms. */
    EXPECT(parse_duration_ms("0") == 0);
    EXPECT(parse_duration_ms("30") == 30000);
    EXPECT(parse_duration_ms("600") == 600000);
}

static void test_parse_duration_with_suffix(void)
{
    EXPECT(parse_duration_ms("30s") == 30000);
    EXPECT(parse_duration_ms("30S") == 30000);
    EXPECT(parse_duration_ms("5m") == 300000);
    EXPECT(parse_duration_ms("5M") == 300000);
    EXPECT(parse_duration_ms("2h") == 7200000);
    EXPECT(parse_duration_ms("2H") == 7200000);
    /* `ms` must beat bare `m` so "250ms" isn't parsed as 250min + 's'. */
    EXPECT(parse_duration_ms("250ms") == 250);
    EXPECT(parse_duration_ms("250MS") == 250);
}

static void test_parse_duration_whitespace(void)
{
    EXPECT(parse_duration_ms("5 m") == 300000);
    EXPECT(parse_duration_ms("2h ") == 7200000);
    EXPECT(parse_duration_ms("100 ms") == 100);
}

static void test_parse_duration_invalid(void)
{
    EXPECT(parse_duration_ms(NULL) == -1);
    EXPECT(parse_duration_ms("") == -1);
    EXPECT(parse_duration_ms("abc") == -1);
    EXPECT(parse_duration_ms("5d") == -1);    /* days not supported */
    EXPECT(parse_duration_ms("-5") == -1);    /* negative rejected */
    EXPECT(parse_duration_ms("5 m x") == -1); /* trailing garbage */
    EXPECT(parse_duration_ms("5mm") == -1);
    EXPECT(parse_duration_ms("5msx") == -1); /* trailing after ms */
    /* strtol clamps to LONG_MAX with ERANGE; the ms suffix has mul==1
     * and would otherwise bypass the overflow guard. */
    EXPECT(parse_duration_ms("99999999999999999999ms") == -1);
    EXPECT(parse_duration_ms("99999999999999999999") == -1);
}

int main(void)
{

    test_uuid_v4_format();
    test_uuid_v4_unique();

    test_buf_append_and_steal();
    test_buf_reset_keeps_capacity();
    test_buf_growth_crosses_default_cap();

    test_expand_home_null();
    test_expand_home_no_tilde();
    test_expand_home_tilde_only();
    test_expand_home_tilde_slash();
    test_expand_home_no_home_env();

    test_slurp_missing();
    test_slurp_empty();
    test_slurp_normal();
    test_slurp_directory_rejected();
    test_slurp_fifo_rejected_no_hang();
    test_slurp_capped_missing();
    test_slurp_capped_under();
    test_slurp_capped_over();
    test_slurp_capped_exact();

    test_cap_lines_no_long_lines();
    test_cap_lines_empty();
    test_cap_lines_truncates_long_line();
    test_cap_lines_preserves_short_neighbors();
    test_cap_lines_long_line_no_trailing_newline();

    test_parse_duration_plain_seconds();
    test_parse_duration_with_suffix();
    test_parse_duration_whitespace();
    test_parse_duration_invalid();

    T_REPORT();
}
