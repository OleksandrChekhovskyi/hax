/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buf.h"
#include "harness.h"
#include "tool.h"
#include "xalloc.h"
#include "system/fd.h"
#include "tools/output_cap.h"

static char *create_temp_file(const void *data, size_t len)
{
    char *dir = t_tempdir();
    char *path = xasprintf("%s/input", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        FAIL("creating %s: %s", path, strerror(errno));
        free(path);
        return NULL;
    }
    if (fd_write_all(fd, data, len) < 0)
        FAIL("writing %s: %s", path, strerror(errno));
    close(fd);
    return path;
}

static char *call_read(const char *args_json)
{
    return TOOL_READ.run(args_json, NULL);
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
    char *path = create_temp_file(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "     1" READ_LINE_DELIM "hello\n     2" READ_LINE_DELIM "world\n");
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
    char *path = create_temp_file(content, sizeof(content));
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "     1" READ_LINE_DELIM "a\xEF\xBF\xBD"
                       "b");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_refuses_binary(void)
{
    const char content[] = {'a', 0x00, 'b'};
    char *path = create_temp_file(content, sizeof(content));
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
    /* Unsliced reads reject files larger than the configured output cap. */
    size_t over = 256 * 1024 + 32;
    char *big = xmalloc(over);
    /* Multi-line so binary detection doesn't fire. */
    for (size_t i = 0; i < over; i++)
        big[i] = (i % 80 == 79) ? '\n' : 'q';
    char *path = create_temp_file(big, over);
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
    /* An explicit slice permits streaming a file larger than the output cap. */
    size_t over = 256 * 1024 + 32;
    char *big = xmalloc(over);
    for (size_t i = 0; i < over; i++)
        big[i] = (i % 80 == 79) ? '\n' : 'q';
    char *path = create_temp_file(big, over);
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
    char *path = create_temp_file(content, sizeof(content) - 1);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":2,\"limit\":2}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "     2" READ_LINE_DELIM "two\n     3" READ_LINE_DELIM "three\n");
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"offset\":4}", path);
    out = call_read(args);
    EXPECT_STR_EQ(out, "     4" READ_LINE_DELIM "four\n     5" READ_LINE_DELIM "five\n");
    free(out);
    free(args);

    args = xasprintf("{\"path\":\"%s\",\"limit\":1}", path);
    out = call_read(args);
    EXPECT_STR_EQ(out, "     1" READ_LINE_DELIM "one\n");
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_offset_past_eof(void)
{
    const char content[] = "one\ntwo\n";
    char *path = create_temp_file(content, sizeof(content) - 1);
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
    char *path = create_temp_file(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":2}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "     2" READ_LINE_DELIM "beta");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_offset_validation(void)
{
    const char content[] = "x\n";
    char *path = create_temp_file(content, sizeof(content) - 1);

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
    /* The output cap limits returned bytes, not how far the reader may seek by line. */
    size_t n_lines = 100 * 1024;
    size_t doc_len = n_lines * 4;
    char *doc = xmalloc(doc_len);
    for (size_t i = 0; i < n_lines; i++) {
        doc[i * 4 + 0] = 'a' + (char)(i % 26);
        doc[i * 4 + 1] = 'b';
        doc[i * 4 + 2] = 'c';
        doc[i * 4 + 3] = '\n';
    }
    char *path = create_temp_file(doc, doc_len);
    free(doc);

    long which = 90000;
    char *args = xasprintf("{\"path\":\"%s\",\"offset\":%ld,\"limit\":1}", path, which);
    char *out = call_read(args);
    char expected[32];
    snprintf(expected, sizeof(expected), "%6ld" READ_LINE_DELIM "%cbc\n", which,
             'a' + (char)((which - 1) % 26));
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
    /* A slice of an enormous line is reduced by the per-line cap. */
    size_t huge_line_len = 300 * 1024;
    char *doc = xmalloc(huge_line_len + 1);
    memset(doc, 'q', huge_line_len);
    doc[huge_line_len] = '\n';
    char *path = create_temp_file(doc, huge_line_len + 1);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":1}", path);
    char *out = call_read(args);
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
     * line-number prefix adds 9 bytes per line (6-wide number + 3-byte →),
     * so leave headroom for the overhead: 800 × 256 = 200 KiB file →
     * ~207 KiB output. */
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
    char *path = create_temp_file(doc, total);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "[truncated") == NULL);
    EXPECT(strstr(out, "bytes elided") == NULL);
    EXPECT(strstr(out, "     1" READ_LINE_DELIM) == out);
    /* Pin the exact size so the format is locked in: each line of pure
     * ASCII under the per-line width cap adds exactly the 9-byte
     * "%6ld" + → prefix and nothing else (no UTF-8 substitution, no
     * line-cap elision). */
    EXPECT(strlen(out) == total + lines * 9);
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
    char *path = create_temp_file(doc, total);
    free(doc);

    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    /* The pre-stat refusal text is the only path that mentions "cap is";
     * the streaming truncation marker doesn't. Its absence proves the
     * `>` boundary held. */
    EXPECT(strstr(out, "cap is") == NULL);
    EXPECT(strstr(out, "     1" READ_LINE_DELIM) == out);
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
    struct buf b;
    buf_init(&b);
    buf_append_str(&b, "short before\n");
    char filler[3000];
    memset(filler, 'x', sizeof(filler));
    buf_append(&b, filler, sizeof(filler));
    buf_append_str(&b, "\nshort after\n");
    char *path = create_temp_file(b.data, b.len);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":3}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "short before\n") != NULL);
    EXPECT(strstr(out, "short after\n") != NULL);
    EXPECT(strstr(out, "bytes elided") != NULL);
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
    char *path = create_temp_file(doc, doc_len);
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
    char *path = create_temp_file(doc, doc_len);
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
    char *path = create_temp_file(doc, doc_len);
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
    /* Unterminated lines count toward the total even when skipped before the offset. */
    const char content[] = "abc";
    char *path = create_temp_file(content, sizeof(content) - 1);
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
    /* Opening a FIFO without a writer could block indefinitely. */
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
    char *path = create_temp_file(doc.buf, doc.len);
    free(doc.buf);

    char *args = xasprintf("{\"path\":\"%s\",\"offset\":1,\"limit\":1}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "     1" READ_LINE_DELIM "ln_\n");
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
    char *path = create_temp_file("", 0);
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
    char *out = TOOL_READ.display.format_extra("{\"path\":\"x\"}");
    EXPECT(out == NULL || *out == '\0');
    free(out);

    out = TOOL_READ.display.format_extra("{\"path\":\"x\",\"offset\":5,\"limit\":10}");
    EXPECT_STR_EQ(out, ":5-14");
    free(out);

    out = TOOL_READ.display.format_extra("{\"path\":\"x\",\"offset\":3}");
    EXPECT_STR_EQ(out, ":3-");
    free(out);

    out = TOOL_READ.display.format_extra("{\"path\":\"x\",\"limit\":7}");
    EXPECT_STR_EQ(out, ":1-7");
    free(out);

    /* Adversarial: offset+limit would overflow LONG_MAX. End must clamp
     * rather than wrap (which would produce a negative number / UB). */
    char *args = xasprintf("{\"path\":\"x\",\"offset\":%ld,\"limit\":2}", LONG_MAX);
    out = TOOL_READ.display.format_extra(args);
    char expected[64];
    snprintf(expected, sizeof(expected), ":%ld-%ld", LONG_MAX, LONG_MAX);
    EXPECT_STR_EQ(out, expected);
    free(out);
    free(args);

    out = TOOL_READ.display.format_extra("{\"path\":\"x\",\"offset\":3,\"limit\":0}");
    EXPECT_STR_EQ(out, ":3-");
    free(out);

    out = TOOL_READ.display.format_extra("{\"path\":\"x.PNG\",\"offset\":1,\"limit\":1}");
    EXPECT(out == NULL);
    free(out);
}

/* A genuinely decodable 2x3 RGB PNG (IHDR + IDAT + IEND); `magick identify`
 * accepts it. The read tool never decodes, but attachment requires a complete
 * container, so the fixture must be a real image, not a header stub. */
static const unsigned char TINY_PNG[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x08, 0x02, 0x00, 0x00, 0x00, 0x36, 0x88, 0x49,
    0xd6, 0x00, 0x00, 0x00, 0x15, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x80, 0x00, 0x8d,
    0x80, 0x0a, 0x20, 0x62, 0x08, 0x58, 0xf0, 0x01, 0x88, 0x00, 0x1f, 0x05, 0x05, 0xa1, 0xfc, 0xf8,
    0x4b, 0x42, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

static void test_read_image_attached(void)
{
    struct tool_run_ctx ctx = {.image_input = 1};
    char *path = create_temp_file(TINY_PNG, sizeof(TINY_PNG));
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = TOOL_READ.run(args, &ctx);
    EXPECT(strstr(out, "Read image") != NULL);
    EXPECT(strstr(out, "image/png") != NULL);
    EXPECT(strstr(out, "2x3") != NULL);

    EXPECT(ctx.n_result_images == 1);
    EXPECT(ctx.result_images != NULL);
    EXPECT_STR_EQ(ctx.result_images[0].mime, "image/png");
    EXPECT(ctx.result_images[0].width == 2 && ctx.result_images[0].height == 3);
    EXPECT(ctx.result_images[0].data_b64 &&
           strlen(ctx.result_images[0].data_b64) == (sizeof(TINY_PNG) + 2) / 3 * 4);
    free(ctx.result_images[0].mime);
    free(ctx.result_images[0].data_b64);
    free(ctx.result_images);

    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_image_model_without_vision(void)
{
    struct tool_run_ctx ctx = {.image_input = 0};
    char *path = create_temp_file(TINY_PNG, sizeof(TINY_PNG));
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = TOOL_READ.run(args, &ctx);
    EXPECT(strstr(out, "does not accept image input") != NULL);
    EXPECT(ctx.n_result_images == 0);
    EXPECT(ctx.result_images == NULL);
    free(out);
    free(args);

    /* NULL ctx means "nowhere to attach" and must behave the same way,
     * not crash or leak. */
    args = xasprintf("{\"path\":\"%s\"}", path);
    out = call_read(args);
    EXPECT(strstr(out, "does not accept image input") != NULL);
    free(out);
    free(args);

    unlink(path);
    free(path);
}

static void test_read_image_oversize_quotes_path(void)
{
    /* A complete, decodable 1x9000 PNG: it trips the per-side cap, and being
     * a valid image it reaches the size check rather than the malformed one.
     * The filename carries a single quote: the downscale hint embeds the path
     * in shell, so it must escape rather than let the quote break out. */
    unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x23, 0x28, 0x08, 0x02, 0x00, 0x00,
        0x00, 0x4e, 0x81, 0x24, 0x21, 0x00, 0x00, 0x00, 0x40, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xda, 0xed, 0xc3, 0x31, 0x0d, 0x00, 0x00, 0x08, 0x03, 0xb0, 0x49, 0x43, 0x1a, 0xd2,
        0x26, 0x0d, 0x1b, 0x1c, 0x6d, 0xd2, 0xcc, 0x36, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
        0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
        0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
        0xea, 0x8b, 0x07, 0x06, 0xa5, 0xbf, 0x0d, 0x5d, 0x0d, 0xf5, 0x28, 0x00, 0x00, 0x00,
        0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    char *dir = t_tempdir();
    char *path = xasprintf("%s/a'b.png", dir);
    FILE *f = fopen(path, "wb");
    EXPECT(f != NULL);
    if (f) {
        fwrite(png, 1, sizeof(png), f);
        fclose(f);
    }
    struct tool_run_ctx ctx = {.image_input = 1};
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = TOOL_READ.run(args, &ctx);
    EXPECT(strstr(out, "per side") != NULL);
    EXPECT(ctx.n_result_images == 0);
    /* The descriptive prefix ("<path> is WxH") shows the path verbatim, but
     * the suggested shell command must quote it: shell_single_quote turns
     * a'b into a'\''b. Only assert when a resize tool was found and a
     * command was emitted (the no-tool branch has no command to check). */
    if (strstr(out, "e.g.:"))
        EXPECT(strstr(out, "a'\\''b.png") != NULL);
    free(out);
    free(args);
    free(path);
}

/* Recognized image signatures that are not complete images must be refused,
 * not attached: providers reject undecodable bytes and an attached one would
 * persist in history and re-fail every turn. The refusal stays recoverable. */
static void refuse_incomplete(const unsigned char *bytes, size_t len)
{
    struct tool_run_ctx ctx = {.image_input = 1};
    char *path = create_temp_file(bytes, len);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = TOOL_READ.run(args, &ctx);
    EXPECT(strstr(out, "truncated or malformed") != NULL);
    EXPECT(strstr(out, "image/png") != NULL);
    EXPECT(ctx.n_result_images == 0);
    EXPECT(ctx.result_images == NULL);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_image_malformed_not_attached(void)
{
    /* A bare signature has no dimensions. */
    static const unsigned char SIG_ONLY[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    refuse_incomplete(SIG_ONLY, sizeof(SIG_ONLY));

    /* Valid header and dimensions but a truncated body (no IEND) — the
     * classic partial download. Dims parse fine, so only the completeness
     * check catches it. TINY_PNG minus its 12-byte IEND chunk. */
    refuse_incomplete(TINY_PNG, sizeof(TINY_PNG) - 12);
}

static void expect_collapsed_path(const char *path, const char *expected)
{
    char *shown = TOOL_READ.display.collapse_argument(path);
    EXPECT_STR_EQ(shown, expected);
    free(shown);
}

static void test_read_collapse_ordinary_names_stay_basenames(void)
{
    expect_collapsed_path("src/render/markdown.c", "markdown.c");
    expect_collapsed_path("agent.c", "agent.c");
    expect_collapsed_path("/home/user/project/src/Turn.c", "Turn.c");
    expect_collapsed_path(NULL, "?");
    expect_collapsed_path("", "?");
    expect_collapsed_path("src/", "src/");
}

static void test_read_collapse_generic_names_keep_parent(void)
{
    expect_collapsed_path("skills/commit-helper/SKILL.md", ".../commit-helper/SKILL.md");
    expect_collapsed_path("docs/guides/README.md", ".../guides/README.md");
    expect_collapsed_path("subprojects/jansson/meson.build", ".../jansson/meson.build");
    expect_collapsed_path("a/pkg/__init__.py", ".../pkg/__init__.py");
    expect_collapsed_path("src/components/Button/index.tsx", ".../Button/index.tsx");
    expect_collapsed_path("project/cmd/serve/main.go", ".../serve/main.go");
    expect_collapsed_path("app/src/main/AndroidManifest.xml", ".../main/AndroidManifest.xml");
    expect_collapsed_path("deep/tree/gnumakefile", ".../tree/gnumakefile");
    expect_collapsed_path("services/api/.env", ".../api/.env");
    expect_collapsed_path("/home/user/.claude/skills/commit/SKILL.md", ".../commit/SKILL.md");
}

static void test_read_collapse_short_paths_shown_whole(void)
{
    expect_collapsed_path("tests/meson.build", "tests/meson.build");
    expect_collapsed_path("README.md", "README.md");
    expect_collapsed_path("/etc/Makefile", "/etc/Makefile");
    expect_collapsed_path("a//SKILL.md", "SKILL.md");
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
    test_read_collapse_ordinary_names_stay_basenames();
    test_read_collapse_generic_names_keep_parent();
    test_read_collapse_short_paths_shown_whole();
    test_read_image_attached();
    test_read_image_model_without_vision();
    test_read_image_oversize_quotes_path();
    test_read_image_malformed_not_attached();
    T_REPORT();
}
