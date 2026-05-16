/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "harness.h"
#include "tool.h"
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
    return TOOL_READ.run(args_json, NULL, NULL);
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
    /* `cat -n` format: each line gets a right-aligned 6-char line-number
     * column followed by a tab, then the original content. */
    const char content[] = "hello\nworld\n";
    char *path = write_tmp(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "     1\thello\n     2\tworld\n");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_sanitizes_utf8(void)
{
    /* An invalid UTF-8 leading byte must become U+FFFD. (Embedded NULs
     * are caught by the binary-file guard and tested separately.) */
    const char content[] = {'a', (char)0xFF, 'b'};
    char *path = write_tmp(content, sizeof(content));
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "     1\ta\xEF\xBF\xBD"
                       "b");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_refuses_binary(void)
{
    /* A NUL byte in the first chunk marks the file as binary; the model
     * gets a clear refusal instead of 256 KiB of replacement characters. */
    const char content[] = {'a', 0x00, 'b'};
    char *path = write_tmp(content, sizeof(content));
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "appears to be binary") != NULL);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_refuses_oversize_no_slice(void)
{
    /* READ_CAP is 256 KiB. Without offset/limit the tool refuses upfront
     * and tells the model how big the file is, so it can ask for a slice
     * or grep via bash instead of accepting a silently truncated prefix. */
    size_t over = 256 * 1024 + 32;
    char *big = xmalloc(over);
    /* Multi-line so binary detection doesn't fire. */
    for (size_t i = 0; i < over; i++)
        big[i] = (i % 80 == 79) ? '\n' : 'q';
    char *path = write_tmp(big, over);
    free(big);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "262176 bytes") != NULL);
    EXPECT(strstr(out, "offset/limit") != NULL);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_oversize_with_slice_ok(void)
{
    /* The pre-stat refusal only fires when neither offset nor limit is
     * provided. With a slice request, streaming proceeds — the model has
     * told us it knows what window it wants. */
    size_t over = 256 * 1024 + 32;
    char *big = xmalloc(over);
    for (size_t i = 0; i < over; i++)
        big[i] = (i % 80 == 79) ? '\n' : 'q';
    char *path = write_tmp(big, over);
    free(big);
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":1}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "262176 bytes") == NULL);
    EXPECT(strstr(out, "offset/limit") == NULL);
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
    EXPECT_STR_EQ(out, "     2\ttwo\n     3\tthree\n");
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"offset\":4}", path);
    out = call_read(args);
    EXPECT_STR_EQ(out, "     4\tfour\n     5\tfive\n");
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"limit\":1}", path);
    out = call_read(args);
    EXPECT_STR_EQ(out, "     1\tone\n");
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
    EXPECT_STR_EQ(out, "     2\tbeta");
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
    char expected[32];
    snprintf(expected, sizeof(expected), "%6ld\t%cbc\n", which, 'a' + (char)((which - 1) % 26));
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
    /* A 300 KiB single-line file with explicit slice request: the
     * pre-stat refusal is bypassed (offset/limit given), streaming
     * proceeds, and the per-line cap kicks in — the result is the
     * first MAX_LINE_LEN bytes of the line plus an inline elision
     * marker. The model can fall back to bash if it needs the rest. */
    size_t huge_line_len = 300 * 1024;
    char *doc = xmalloc(huge_line_len + 1);
    memset(doc, 'q', huge_line_len);
    doc[huge_line_len] = '\n';
    char *path = write_tmp(doc, huge_line_len + 1);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":1}", path);
    char *out = call_read(args);
    /* Per-line cap is OUTPUT_CAP_LINE_WIDTH (500); result is well under
     * the byte cap and shows the inline elision marker. */
    EXPECT(strlen(out) < 1000);
    EXPECT(strstr(out, "bytes elided") != NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_no_false_marker_when_under_cap(void)
{
    /* A multi-line file comfortably under the byte cap reads in full
     * without a truncation marker, and the output starts with the
     * line-1 prefix (i.e., the pre-stat refusal didn't fire either —
     * that path would have produced a "cap is" error instead). The
     * `cat -n` prefix adds 7 bytes per line, so leave headroom for the
     * overhead: 800 × 256 = 200 KiB file → ~205 KiB output. */
    size_t lines = 800;
    size_t line_len = 256;
    size_t total = lines * line_len;
    char *doc = xmalloc(total);
    /* Line content fits the per-line width cap (500). Each line is 255
     * 'q' bytes followed by a '\n'. */
    for (size_t i = 0; i < lines; i++) {
        memset(doc + i * line_len, 'q', line_len - 1);
        doc[i * line_len + line_len - 1] = '\n';
    }
    char *path = write_tmp(doc, total);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "[truncated") == NULL);
    EXPECT(strstr(out, "bytes elided") == NULL);
    EXPECT(strstr(out, "     1\t") == out);
    /* Pin the exact size so the format is locked in: each line of pure
     * ASCII under the per-line width cap adds exactly the 7-byte
     * "%6ld\t" prefix and nothing else (no UTF-8 substitution, no
     * line-cap elision). */
    EXPECT(strlen(out) == total + lines * 7);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_pre_stat_boundary_st_size_at_cap(void)
{
    /* The pre-stat refusal uses `>`, not `>=`, so a file whose on-disk
     * size lands exactly at the byte cap is allowed to stream. With
     * line-number prefixes inflating the output past the cap, streaming
     * itself trips TRUNC_BYTES — but the test of the pre-stat boundary
     * is that we get streamed content + a truncation marker, not the
     * "is X bytes; cap is Y" refusal. 1024 × 256 = 262144 = 256 KiB. */
    size_t lines = 1024;
    size_t line_len = 256;
    size_t total = lines * line_len;
    char *doc = xmalloc(total);
    for (size_t i = 0; i < lines; i++) {
        memset(doc + i * line_len, 'q', line_len - 1);
        doc[i * line_len + line_len - 1] = '\n';
    }
    char *path = write_tmp(doc, total);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    /* The pre-stat refusal text is the only path that mentions "cap is";
     * the streaming truncation marker doesn't. Its absence proves the
     * `>` boundary held. */
    EXPECT(strstr(out, "cap is") == NULL);
    EXPECT(strstr(out, "     1\t") == out);
    /* Prefixes inflate the output past the cap, so the stream-side
     * truncation marker is expected. */
    EXPECT(strstr(out, "[truncated at") != NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_caps_long_line(void)
{
    /* A single line longer than OUTPUT_CAP_LINE_WIDTH (500) gets truncated
     * with an inline marker; surrounding short lines are left alone. */
    struct buf b;
    buf_init(&b);
    buf_append_str(&b, "short before\n");
    char filler[3000];
    memset(filler, 'x', sizeof(filler));
    buf_append(&b, filler, sizeof(filler));
    buf_append_str(&b, "\nshort after\n");
    char *path = write_tmp(b.data, b.len);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":3}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "short before\n") != NULL);
    EXPECT(strstr(out, "short after\n") != NULL);
    EXPECT(strstr(out, "bytes elided") != NULL);
    /* The 3000-x line is reduced to ~500 + a short marker. */
    EXPECT(strlen(out) < 700);
    free(out);
    free(args);

    buf_free(&b);
    unlink(path);
    free(path);
}

static void test_read_exact_line_cap_no_false_marker(void)
{
    /* A file with exactly OUTPUT_CAP_LINES lines (all newline-terminated)
     * fits the cap exactly — nothing past line 2000. The truncation
     * marker must NOT fire, otherwise the model sees a misleading "file
     * has more" hint. */
    size_t n_lines = 2000;
    size_t doc_len = n_lines * 2;
    char *doc = xmalloc(doc_len);
    for (size_t i = 0; i < n_lines; i++) {
        doc[i * 2] = 'x';
        doc[i * 2 + 1] = '\n';
    }
    char *path = write_tmp(doc, doc_len);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "[truncated") == NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_unterminated_final_line_at_cap(void)
{
    /* OUTPUT_CAP_LINES + 1 logical lines where the last line has no
     * trailing '\n' (2000 newlines + 1 partial). The cap fires at the
     * 2000th newline; the peek must detect the partial 2001st line and
     * mark TRUNC_LINES. */
    size_t n_lines = 2000;
    size_t doc_len = n_lines * 2 + 5; /* "x\n" × 2000 + "abcde" */
    char *doc = xmalloc(doc_len);
    for (size_t i = 0; i < n_lines; i++) {
        doc[i * 2] = 'x';
        doc[i * 2 + 1] = '\n';
    }
    memcpy(doc + n_lines * 2, "abcde", 5);
    char *path = write_tmp(doc, doc_len);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "[truncated at 2000 lines") != NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_caps_implicit_line_count(void)
{
    /* A file with many short lines has small total bytes but should
     * still trigger the shared OUTPUT_CAP_LINES guardrail when no
     * explicit limit is given. main() pins HAX_TOOL_OUTPUT_CAP=256K,
     * so 5000 two-byte lines (10 KiB) sit comfortably under the byte
     * cap; without the line cap, all 5000 lines would flow back. */
    size_t n_lines = 5000;
    size_t doc_len = n_lines * 2;
    char *doc = xmalloc(doc_len);
    for (size_t i = 0; i < n_lines; i++) {
        doc[i * 2] = 'x';
        doc[i * 2 + 1] = '\n';
    }
    char *path = write_tmp(doc, doc_len);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "[truncated at 2000 lines") != NULL);
    EXPECT(strstr(out, "offset/limit") != NULL);
    free(out);
    free(args);

    /* A tighter explicit limit (below the shared cap) is the model's
     * window — line-cap marker must NOT fire and only the requested
     * lines come back. */
    args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":1500}", path);
    out = call_read(args);
    EXPECT(strstr(out, "[truncated at") == NULL);
    free(out);
    free(args);

    /* An explicit limit *above* the shared cap can't lift the cap —
     * line cap is an absolute ceiling, not a default. The model gets
     * only OUTPUT_CAP_LINES lines and a TRUNC_LINES marker, even
     * though byte cap doesn't fire (5000 × 2 = 10 KiB). */
    args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":3000}", path);
    out = call_read(args);
    EXPECT(strstr(out, "[truncated at 2000 lines") != NULL);
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
    EXPECT_STR_EQ(out, "     1\tln_\n");
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
    /* The byte cap is the env-tunable knob; pin it to 256K so the tests
     * below (most of which use multi-100K fixtures) exercise the code
     * path the assertions describe regardless of the compiled-in default. */
    setenv("HAX_TOOL_OUTPUT_CAP", "256k", 1);

    test_read_invalid_json();
    test_read_missing_path();
    test_read_empty_path();
    test_read_nonexistent();
    test_read_normal();
    test_read_sanitizes_utf8();
    test_read_refuses_binary();
    test_read_refuses_oversize_no_slice();
    test_read_oversize_with_slice_ok();
    test_read_caps_long_line();
    test_read_caps_implicit_line_count();
    test_read_exact_line_cap_no_false_marker();
    test_read_unterminated_final_line_at_cap();
    test_read_offset_limit();
    test_read_offset_past_eof();
    test_read_no_trailing_newline();
    test_read_offset_validation();
    test_read_offset_one_on_empty();
    test_read_bounded_slice_suppresses_truncation_marker();
    test_read_range_past_cap_in_large_file();
    test_read_first_line_larger_than_cap();
    test_read_no_false_marker_when_under_cap();
    test_read_pre_stat_boundary_st_size_at_cap();
    test_read_past_eof_counts_trailing_line_in_skip_mode();
    test_read_refuses_special_file();
    test_read_display_extra();
    T_REPORT();
}
