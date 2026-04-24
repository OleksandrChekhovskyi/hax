/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <stdlib.h>
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

/* U+FFFD in UTF-8 */
#define REPL "\xEF\xBF\xBD"

/* ---------- sanitize_utf8 ---------- */

static void test_sanitize_ascii(void)
{
    char *out = sanitize_utf8("hello", 5);
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_sanitize_empty(void)
{
    char *out = sanitize_utf8("", 0);
    EXPECT(out != NULL);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_sanitize_nul_byte_replaced(void)
{
    char *out = sanitize_utf8("a\0b", 3);
    EXPECT_MEM_EQ(out, strlen(out), "a" REPL "b", 5);
    free(out);
}

static void test_sanitize_valid_multibyte_preserved(void)
{
    /* "é€🎉" = C3 A9 | E2 82 AC | F0 9F 8E 89 */
    const char in[] = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\x89";
    char *out = sanitize_utf8(in, sizeof(in) - 1);
    EXPECT_STR_EQ(out, in);
    free(out);
}

static void test_sanitize_overlong_two_byte_nul(void)
{
    /* C0 80 — overlong NUL. 0xC0 matches the 2-byte pattern so we parse
     * the pair, reject on the cp<0x80 strict check, advance one byte;
     * the trailing 0x80 is then an orphan continuation, also replaced. */
    char *out = sanitize_utf8("\xC0\x80", 2);
    EXPECT_STR_EQ(out, REPL REPL);
    free(out);
}

static void test_sanitize_overlong_three_byte(void)
{
    /* E0 80 80 — overlong encoding of U+0000 */
    char *out = sanitize_utf8("\xE0\x80\x80", 3);
    EXPECT_STR_EQ(out, REPL REPL REPL);
    free(out);
}

static void test_sanitize_surrogate_rejected(void)
{
    /* ED A0 80 = U+D800, high surrogate */
    char *out = sanitize_utf8("\xED\xA0\x80", 3);
    EXPECT_STR_EQ(out, REPL REPL REPL);
    free(out);
}

static void test_sanitize_above_max_codepoint(void)
{
    /* F4 90 80 80 = U+110000, beyond U+10FFFF */
    char *out = sanitize_utf8("\xF4\x90\x80\x80", 4);
    EXPECT_STR_EQ(out, REPL REPL REPL REPL);
    free(out);
}

static void test_sanitize_truncated_tail(void)
{
    /* 0xC2 starts a 2-byte sequence but there's no continuation byte. */
    char *out = sanitize_utf8("\xC2", 1);
    EXPECT_STR_EQ(out, REPL);
    free(out);
}

static void test_sanitize_invalid_continuation(void)
{
    /* C2 20 — 0x20 is ASCII, not a continuation byte. */
    char *out = sanitize_utf8("\xC2 ", 2);
    EXPECT_STR_EQ(out, REPL " ");
    free(out);
}

static void test_sanitize_invalid_leading_byte(void)
{
    /* 0xFF is not a valid UTF-8 leading byte at all. */
    char *out = sanitize_utf8("x\xFFy", 3);
    EXPECT_STR_EQ(out, "x" REPL "y");
    free(out);
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

int main(void)
{
    test_sanitize_ascii();
    test_sanitize_empty();
    test_sanitize_nul_byte_replaced();
    test_sanitize_valid_multibyte_preserved();
    test_sanitize_overlong_two_byte_nul();
    test_sanitize_overlong_three_byte();
    test_sanitize_surrogate_rejected();
    test_sanitize_above_max_codepoint();
    test_sanitize_truncated_tail();
    test_sanitize_invalid_continuation();
    test_sanitize_invalid_leading_byte();

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
    test_slurp_capped_missing();
    test_slurp_capped_under();
    test_slurp_capped_over();
    test_slurp_capped_exact();

    T_REPORT();
}
