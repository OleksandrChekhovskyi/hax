/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "harness.h"
#include "util.h"

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

static char *call_read(const char *args_json)
{
    return TOOL_READ.run(args_json);
}

static void test_read_invalid_json(void)
{
    char *out = call_read("not json");
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_read_missing_path(void)
{
    char *out = call_read("{}");
    EXPECT(strstr(out, "missing 'path'") != NULL);
    free(out);
}

static void test_read_empty_path(void)
{
    char *out = call_read("{\"path\":\"\"}");
    EXPECT(strstr(out, "missing 'path'") != NULL);
    free(out);
}

static void test_read_nonexistent(void)
{
    char *out = call_read("{\"path\":\"/nonexistent/path/should-not-exist\"}");
    EXPECT(strstr(out, "error reading") != NULL);
    free(out);
}

static void test_read_normal(void)
{
    const char content[] = "hello\nworld\n";
    char *path = write_tmp(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, content);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_sanitizes_utf8(void)
{
    /* Embed a raw NUL and an invalid leading byte — both must become U+FFFD. */
    const char content[] = {'a', 0x00, 'b', (char)0xFF, 'c'};
    char *path = write_tmp(content, sizeof(content));
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "a\xEF\xBF\xBD"
                       "b\xEF\xBF\xBD"
                       "c");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_truncation_marker(void)
{
    /* READ_CAP is 256 KiB; write one byte past it and expect the marker. */
    size_t over = 256 * 1024 + 32;
    char *big = xmalloc(over);
    memset(big, 'q', over);
    char *path = write_tmp(big, over);
    free(big);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "[truncated at") != NULL);
    EXPECT(strstr(out, "file is larger]") != NULL);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_offset_limit(void)
{
    const char content[] = "one\ntwo\nthree\nfour\nfive\n";
    char *path = write_tmp(content, sizeof(content) - 1);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":2,\"limit\":2}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "two\nthree\n");
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"offset\":4}", path);
    out = call_read(args);
    EXPECT_STR_EQ(out, "four\nfive\n");
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"limit\":1}", path);
    out = call_read(args);
    EXPECT_STR_EQ(out, "one\n");
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_offset_past_eof(void)
{
    const char content[] = "one\ntwo\n";
    char *path = write_tmp(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":5}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "past EOF") != NULL);
    EXPECT(strstr(out, "file has 2 lines") != NULL);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_no_trailing_newline(void)
{
    const char content[] = "alpha\nbeta";
    char *path = write_tmp(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":2}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "beta");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_offset_validation(void)
{
    const char content[] = "x\n";
    char *path = write_tmp(content, sizeof(content) - 1);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":0}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "'offset' must be >= 1") != NULL);
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"offset\":\"two\"}", path);
    out = call_read(args);
    EXPECT(strstr(out, "'offset' must be an integer") != NULL);
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"limit\":0}", path);
    out = call_read(args);
    EXPECT(strstr(out, "'limit' must be >= 1") != NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_range_past_cap_in_large_file(void)
{
    /* The previous implementation slurped the first 256K and falsely
     * reported "past readable region" for ranges starting after that
     * prefix. With streaming, an offset that lives well past the cap
     * should still be reachable. */
    size_t n_lines = 100 * 1024;
    size_t doc_len = n_lines * 4;
    char *doc = xmalloc(doc_len);
    for (size_t i = 0; i < n_lines; i++) {
        doc[i * 4 + 0] = 'a' + (char)(i % 26);
        doc[i * 4 + 1] = 'b';
        doc[i * 4 + 2] = 'c';
        doc[i * 4 + 3] = '\n';
    }
    char *path = write_tmp(doc, doc_len);
    free(doc);

    /* Line 90000 is far past the old 256K cap. With the fix, we should
     * still get exactly that line. */
    long which = 90000;
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":%ld,\"limit\":1}", path, which);
    char *out = call_read(args);
    char expected[8];
    snprintf(expected, sizeof(expected), "%cbc\n", 'a' + (char)((which - 1) % 26));
    EXPECT_STR_EQ(out, expected);
    EXPECT(strstr(out, "[truncated") == NULL);
    EXPECT(strstr(out, "past readable") == NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_first_line_larger_than_cap(void)
{
    /* Strict cap: memory used by the result is hard-bounded by READ_CAP
     * regardless of line length, so a single 300 KiB line returns the
     * first ~256 KiB plus a truncation marker. The previous "include
     * the first line whole" exception was the OOM door (a 10 GB
     * single-line file would have allocated 10 GB before any cap
     * fired). The model can fall back to bash if it needs the rest. */
    size_t huge_line_len = 300 * 1024;
    char *doc = xmalloc(huge_line_len + 1);
    memset(doc, 'q', huge_line_len);
    doc[huge_line_len] = '\n';
    char *path = write_tmp(doc, huge_line_len + 1);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":1}", path);
    char *out = call_read(args);
    /* Result is roughly the cap, plus a small footer for the marker. */
    EXPECT(strlen(out) > 256 * 1024 - 16);
    EXPECT(strlen(out) < 256 * 1024 + 256);
    EXPECT(strstr(out, "[truncated") != NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_exact_cap_no_false_marker(void)
{
    /* When the file's content lands exactly at READ_CAP, the result is
     * the whole file — no truncation occurred — so the marker must NOT
     * appear. The cap-check-before-append design gets this right
     * because the reader exits via read()==0, not via the cap branch. */
    size_t exact = 256 * 1024;
    char *doc = xmalloc(exact);
    memset(doc, 'q', exact);
    char *path = write_tmp(doc, exact);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strlen(out) == exact);
    EXPECT(strstr(out, "[truncated") == NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_past_eof_counts_trailing_line_in_skip_mode(void)
{
    /* "abc" has no final newline. With offset=2 the reader stays in
     * skip mode and never reaches the collect path that updates
     * saw_data_in_current_line. Without the fix, lines_complete stays
     * at 0 and the past-EOF message reports "file has 0 lines" — wrong,
     * the file has 1. */
    const char content[] = "abc";
    char *path = write_tmp(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":2}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "file has 1 line") != NULL);
    EXPECT(strstr(out, "past EOF") != NULL);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_refuses_special_file(void)
{
    /* A FIFO open(O_RDONLY) without a writer would block read() forever.
     * /dev/zero would stream until cap (correct memory-wise but useless
     * to the model). Stat upfront and refuse. */
    char path[] = "/tmp/hax-test-fifo-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    close(fd);
    unlink(path);
    EXPECT(mkfifo(path, 0644) == 0);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "not a regular file") != NULL);
    free(out);
    free(args);
    unlink(path);
}

static void test_read_bounded_slice_suppresses_truncation_marker(void)
{
    /* Build a >READ_CAP file (256K) with one short line per row so the
     * cap kicks in. With offset=1 limit=1, the requested line is fully
     * covered by what we read; the marker would otherwise leak into the
     * tool result and look like file content. */
    size_t n_lines = 100 * 1024;
    struct {
        char *buf;
        size_t len;
    } doc;
    doc.len = n_lines * 4; /* "ln_\n" pattern, ~400KB > READ_CAP */
    doc.buf = xmalloc(doc.len);
    for (size_t i = 0; i < n_lines; i++) {
        doc.buf[i * 4 + 0] = 'l';
        doc.buf[i * 4 + 1] = 'n';
        doc.buf[i * 4 + 2] = '_';
        doc.buf[i * 4 + 3] = '\n';
    }
    char *path = write_tmp(doc.buf, doc.len);
    free(doc.buf);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":1}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "ln_\n");
    EXPECT(strstr(out, "[truncated") == NULL);
    free(out);
    free(args);

    /* Open-ended slice on the same file *should* keep the marker —
     * the slice naturally hits the cap. */
    args = xasprintf("{\"path\":\"%s\",\"offset\":1}", path);
    out = call_read(args);
    EXPECT(strstr(out, "[truncated") != NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_offset_one_on_empty(void)
{
    /* Default-case sanity: offset=1 on empty file is benign, returns "". */
    char *path = write_tmp("", 0);
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_display_extra(void)
{
    /* No range → no suffix. */
    char *out = TOOL_READ.format_display_extra("{\"path\":\"x\"}");
    EXPECT(out == NULL || *out == '\0');
    free(out);

    /* Both bounds → ":N-M" form. */
    out = TOOL_READ.format_display_extra("{\"path\":\"x\",\"offset\":5,\"limit\":10}");
    EXPECT_STR_EQ(out, ":5-14");
    free(out);

    /* Offset only (no limit) → open-ended ":N-". */
    out = TOOL_READ.format_display_extra("{\"path\":\"x\",\"offset\":3}");
    EXPECT_STR_EQ(out, ":3-");
    free(out);

    /* Limit only → offset defaults to 1. */
    out = TOOL_READ.format_display_extra("{\"path\":\"x\",\"limit\":7}");
    EXPECT_STR_EQ(out, ":1-7");
    free(out);

    /* Adversarial: offset+limit would overflow LONG_MAX. End must clamp
     * rather than wrap (which would produce a negative number / UB). */
    char *args = xasprintf("{\"path\":\"x\",\"offset\":%ld,\"limit\":2}", LONG_MAX);
    out = TOOL_READ.format_display_extra(args);
    char expected[64];
    snprintf(expected, sizeof(expected), ":%ld-%ld", LONG_MAX, LONG_MAX);
    EXPECT_STR_EQ(out, expected);
    free(out);
    free(args);

    /* Garbage limit (<= 0) → fall back to open-ended form. */
    out = TOOL_READ.format_display_extra("{\"path\":\"x\",\"offset\":3,\"limit\":0}");
    EXPECT_STR_EQ(out, ":3-");
    free(out);
}

int main(void)
{
    test_read_invalid_json();
    test_read_missing_path();
    test_read_empty_path();
    test_read_nonexistent();
    test_read_normal();
    test_read_sanitizes_utf8();
    test_read_truncation_marker();
    test_read_offset_limit();
    test_read_offset_past_eof();
    test_read_no_trailing_newline();
    test_read_offset_validation();
    test_read_offset_one_on_empty();
    test_read_bounded_slice_suppresses_truncation_marker();
    test_read_range_past_cap_in_large_file();
    test_read_first_line_larger_than_cap();
    test_read_exact_cap_no_false_marker();
    test_read_past_eof_counts_trailing_line_in_skip_mode();
    test_read_refuses_special_file();
    test_read_display_extra();
    T_REPORT();
}
