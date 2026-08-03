/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "harness.h"
#include "util.h"

static char *write_temp_file(const void *data, size_t length)
{
    char *path = xasprintf("%s/file", t_tempdir());
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        FAIL("open %s: %s", path, strerror(errno));
        free(path);
        return NULL;
    }
    if (write_all(fd, data, length) < 0)
        FAIL("write %s: %s", path, strerror(errno));
    close(fd);
    return path;
}

/* ---------- diagnostics ---------- */

static void test_diag_sequence(void)
{
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    EXPECT(saved >= 0);
    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    EXPECT(dup2(fileno(tmp), STDERR_FILENO) >= 0);

    unsigned long before = hax_diag_sequence();
    hax_warn("sequence test");
    EXPECT(hax_diag_sequence() == before + 1);

    EXPECT(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    fclose(tmp);
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

/* ---------- allocation and buffer ---------- */

static void test_zero_sized_allocations(void)
{
    void *malloc_result = xmalloc(0);
    void *calloc_result = xcalloc(0, SIZE_MAX);
    void *realloc_result = xrealloc(NULL, 0);

    EXPECT(malloc_result != NULL);
    EXPECT(calloc_result != NULL);
    EXPECT(realloc_result != NULL);
    free(malloc_result);
    free(calloc_result);
    free(realloc_result);
}

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

static void test_buf_steal_empty(void)
{
    struct buf buf;
    buf_init(&buf);

    char *contents = buf_steal(&buf);

    EXPECT_STR_EQ(contents, "");
    EXPECT(buf.data == NULL && buf.len == 0 && buf.cap == 0);
    free(contents);
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

static void test_buf_grows_repeatedly(void)
{
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

/* ---------- slurp_file ---------- */

static void test_slurp_missing(void)
{
    size_t n = 999;
    char *p = slurp_file("/nonexistent/path/should-not-exist", &n);
    EXPECT(p == NULL);
}

static void test_slurp_empty(void)
{
    char *path = write_temp_file("", 0);
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
    char *path = write_temp_file(content, clen);
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
    char *dir = t_tempdir();
    errno = 0;
    char *p = slurp_file(dir, NULL);
    EXPECT(p == NULL);
    EXPECT(errno == EISDIR);
}

static void test_slurp_fifo_rejected_no_hang(void)
{
    /* A blocking read-only open on a writer-less FIFO never returns. */
    char *path = t_tempdir();
    char fifo[64];
    snprintf(fifo, sizeof(fifo), "%s/f", path);
    EXPECT(mkfifo(fifo, 0644) == 0);

    errno = 0;
    int fd = open_regular_file(fifo);
    EXPECT(fd < 0);
    EXPECT(errno == EINVAL);

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
    char *path = write_temp_file(content, clen);
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

static void test_slurp_capped_zero(void)
{
    char *path = write_temp_file("x", 1);
    size_t length = 1;
    int truncated = 0;

    char *contents = slurp_file_capped(path, 0, &length, &truncated);

    EXPECT_STR_EQ(contents, "");
    EXPECT(length == 0);
    EXPECT(truncated == 1);
    free(contents);
    free(path);
}

static void test_slurp_capped_does_not_preallocate_cap(void)
{
    char *path = write_temp_file("short", 5);
    size_t length = 0;
    int truncated = 1;

    char *contents = slurp_file_capped(path, SIZE_MAX, &length, &truncated);

    EXPECT_STR_EQ(contents, "short");
    EXPECT(length == 5);
    EXPECT(truncated == 0);
    free(contents);
    free(path);
}

static void test_slurp_capped_over(void)
{
    /* File is cap+100 bytes; we expect cap bytes kept and truncated=1. */
    const size_t cap = 64;
    char big[200];
    memset(big, 'a', sizeof(big));
    char *path = write_temp_file(big, sizeof(big));
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
    char *path = write_temp_file(buf, cap);
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

/* ---------- parse_size ---------- */

static void test_parse_size_basic(void)
{
    EXPECT(parse_size("4096") == 4096);
    EXPECT(parse_size("256k") == 256L * 1024);
    EXPECT(parse_size("128K") == 128L * 1024);
    EXPECT(parse_size("1m") == 1024L * 1024);
    EXPECT(parse_size("1M") == 1024L * 1024);
}

static void test_parse_size_invalid_returns_zero(void)
{
    EXPECT(parse_size(NULL) == 0);
    EXPECT(parse_size("") == 0);
    EXPECT(parse_size("xyz") == 0);
    EXPECT(parse_size("0") == 0);   /* explicit zero is still rejected */
    EXPECT(parse_size("-5k") == 0); /* negative */
    EXPECT(parse_size("5k junk") == 0);
}

static void test_parse_size_rejects_overflow(void)
{
    /* Numerals strtol clamps to LONG_MAX must NOT slip past — caller
     * would otherwise allocate / accept absurd cap values. */
    EXPECT(parse_size("99999999999999999999") == 0);
    EXPECT(parse_size("99999999999999999999k") == 0);
    /* Multiply-overflow guard: a value that fits in long but overflows
     * after the suffix-mul must be rejected. LONG_MAX / 1024 + 1 with
     * a 'k' suffix overflows. On 64-bit long, that's 9007199254740993k. */
    char buf[64];
    snprintf(buf, sizeof(buf), "%ldk", LONG_MAX / 1024L + 1);
    EXPECT(parse_size(buf) == 0);
    snprintf(buf, sizeof(buf), "%ldm", LONG_MAX / (1024L * 1024L) + 1);
    EXPECT(parse_size(buf) == 0);
}

/* ---------- flatten_for_display ---------- */

static void test_flatten_null(void)
{
    char *out = flatten_for_display(NULL);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_flatten_empty(void)
{
    char *out = flatten_for_display("");
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_flatten_plain(void)
{
    char *out = flatten_for_display("ls -la");
    EXPECT_STR_EQ(out, "ls -la");
    free(out);
}

static void test_flatten_newline(void)
{
    char *out = flatten_for_display("ls\npwd");
    EXPECT_STR_EQ(out, "ls pwd");
    free(out);
}

static void test_flatten_collapses_runs(void)
{
    /* Multiple newlines/tabs/spaces collapse to a single space. */
    char *out = flatten_for_display("a\n\n\tb  \r\n c");
    EXPECT_STR_EQ(out, "a b c");
    free(out);
}

static void test_flatten_strips_edges(void)
{
    char *out = flatten_for_display("\n  hello world\n\n");
    EXPECT_STR_EQ(out, "hello world");
    free(out);
}

static void test_flatten_all_whitespace(void)
{
    /* All-whitespace input collapses to empty — leading-trim drops the
     * first run, trailing-trim drops everything that came after. */
    char *out = flatten_for_display("  \n\t\r  ");
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_flatten_control_bytes(void)
{
    /* All ASCII control bytes (incl. DEL 0x7f) collapse to spaces. */
    char *out = flatten_for_display("a\x01\x02\x03"
                                    "b\x7f"
                                    "c");
    EXPECT_STR_EQ(out, "a b c");
    free(out);
}

static void test_display_cells(void)
{
    /* ASCII: cells == bytes. */
    EXPECT(display_cells("abc") == 3);
    EXPECT(display_cells("") == 0);
    EXPECT(display_cells(NULL) == 0);
    /* Multi-byte codepoint occupying one cell: bytes over-count. */
    EXPECT(display_cells("café") == 4);
    /* Wide CJK codepoint: two cells. */
    EXPECT(display_cells("a\xE4\xB8\xAD") == 3);
    /* Combining mark rides on the base glyph (zero cells). */
    EXPECT(display_cells("e\xCC\x81") == 1);
}

static void test_flatten_preserves_high_bytes(void)
{
    /* UTF-8 bytes (>= 0x80) for non-dangerous codepoints pass through. */
    char *out = flatten_for_display("café\nlatte");
    EXPECT_STR_EQ(out, "café latte");
    free(out);
}

static void test_flatten_substitutes_bidi_override(void)
{
    /* Trojan Source: U+202E RIGHT-TO-LEFT OVERRIDE encoded as
     * E2 80 AE. Flatten substitutes with '?' so a model-supplied
     * tool arg can't bidi-reorder the rendered header. */
    char *out = flatten_for_display("ab\xE2\x80\xAE"
                                    "cd");
    EXPECT_STR_EQ(out, "ab?cd");
    free(out);
}

static void test_flatten_substitutes_zwj(void)
{
    /* U+200D ZERO WIDTH JOINER (E2 80 8D). Width-zero invisible —
     * substituted so the displayed string matches the cell budget. */
    char *out = flatten_for_display("ab\xE2\x80\x8D"
                                    "cd");
    EXPECT_STR_EQ(out, "ab?cd");
    free(out);
}

static void test_flatten_substitutes_malformed_utf8(void)
{
    /* Lone continuation byte: malformed UTF-8 → '?'. */
    char *out = flatten_for_display("ab\x80"
                                    "cd");
    EXPECT_STR_EQ(out, "ab?cd");
    free(out);
}

static void test_flatten_caps_zero_width_run(void)
{
    /* Bound bytes consumed by a visually zero-width run. */
    char input[1 + 2 * 100 + 1];
    input[0] = 'a';
    for (int k = 0; k < 100; k++) {
        input[1 + 2 * k] = (char)0xCC;
        input[2 + 2 * k] = (char)0x81;
    }
    input[1 + 2 * 100] = '\0';
    char *out = flatten_for_display(input);
    /* "a" + 8 combining marks = 1 + 16 = 17 bytes. */
    EXPECT(strlen(out) == 17);
    EXPECT(out[0] == 'a');
    free(out);
}

static void test_flatten_preserves_legit_combining_run(void)
{
    /* Below the cap, combining marks pass through unchanged so
     * legitimate decomposed forms (e.g. macOS HFS+ NFD paths,
     * Devanagari with multiple marks per base) render correctly.
     * "a" + 3 combining marks = 1 + 6 = 7 bytes, unchanged. */
    char *out = flatten_for_display("a\xCC\x81\xCC\x81\xCC\x81");
    EXPECT_STR_EQ(out, "a\xCC\x81\xCC\x81\xCC\x81");
    free(out);
}

/* ---------- truncate_for_display ---------- */

static void test_truncate_under_cap(void)
{
    /* String shorter than cap: returned unchanged (still a fresh dup). */
    char *out = truncate_for_display("hello", 10);
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_truncate_exact_cap(void)
{
    /* strlen == cap: still a no-op. */
    char *out = truncate_for_display("hello", 5);
    EXPECT_STR_EQ(out, "hello");
    free(out);
}

static void test_truncate_above_cap(void)
{
    /* strlen > cap: cut to cap-3 bytes and append "...". */
    char *out = truncate_for_display("hello world", 8);
    EXPECT_STR_EQ(out, "hello...");
    free(out);
}

static void test_truncate_tiny_cap(void)
{
    /* cap < 4 has no room for "..." — hard cut, no marker. */
    char *out = truncate_for_display("hello", 3);
    EXPECT_STR_EQ(out, "hel");
    free(out);
}

static void test_truncate_utf8_boundary(void)
{
    /* "café latte" — 10 cells (each codepoint is 1 cell here),
     * 11 bytes (é is 2 bytes). Cap of 5 cells: cut at 2 cells
     * (cap-3) and append "...". Result "ca...". The é is preserved
     * as a unit (no half-codepoint). */
    char *out = truncate_for_display("café latte", 5);
    EXPECT_STR_EQ(out, "ca...");
    free(out);
}

static void test_truncate_keeps_multibyte_intact(void)
{
    /* Cap of 6 cells over "café latte" (10 cells): keeps "caf"
     * (3 cells) plus "..." since cap-3=3. The é codepoint is past
     * the cut and excluded entirely — never split mid-byte. */
    char *out = truncate_for_display("café latte", 6);
    EXPECT_STR_EQ(out, "caf...");
    free(out);
}

static void test_truncate_under_cap_multibyte(void)
{
    /* "café" = 4 cells (each codepoint is 1 cell, é via wcwidth=1)
     * but 5 bytes. cap=4 fits even though byte length exceeds cap —
     * cells, not bytes, drive the decision. */
    char *out = truncate_for_display("café", 4);
    EXPECT_STR_EQ(out, "café");
    free(out);
}

static void test_truncate_keeps_trailing_combining_mark(void)
{
    /* "abcd" + COMBINING ACUTE on d (U+0301) = 4 cells, 6 bytes.
     * cap=4 should fit it whole — combining marks contribute 0 cells
     * and ride on the prior glyph. Without absorbing trailing zero-
     * width codepoints, advance_cells stops after 'd' and the cut
     * orphans the combining mark, mis-rendering as "a..." or similar. */
    char *out = truncate_for_display("abcd\xCC\x81", 4);
    EXPECT_STR_EQ(out, "abcd\xCC\x81");
    free(out);
}

static void test_truncate_emoji_two_cells(void)
{
    /* Wide codepoints take 2 cells. "🦀abc" = 1 emoji (2 cells) +
     * 3 ASCII = 5 cells, 7 bytes. cap=4 means the emoji + 1 char fits
     * with room for nothing else; we cut at 1 cell (emoji is 2 → can't
     * include) and append "...". cap-3=1 cell budget → cut after the
     * first ASCII codepoint that fits, i.e. nothing fits, hard cut at
     * byte 0 + "...". Result "...". */
    char *out = truncate_for_display("\xF0\x9F\xA6\x80"
                                     "abc",
                                     4);
    EXPECT_STR_EQ(out, "...");
    free(out);

    /* cap=5 fits the full string (2+1+1+1 = 5 cells). */
    char *out2 = truncate_for_display("\xF0\x9F\xA6\x80"
                                      "abc",
                                      5);
    EXPECT_STR_EQ(out2, "\xF0\x9F\xA6\x80"
                        "abc");
    free(out2);
}

static void test_truncate_null(void)
{
    char *out = truncate_for_display(NULL, 10);
    EXPECT_STR_EQ(out, "");
    free(out);
}

/* ---------- wrap_break_pos ---------- */

static void test_wrap_break_fits(void)
{
    /* Whole string fits: end == resume == len, no break. */
    size_t resume = 999;
    size_t end = wrap_break_pos("hello", 5, 10, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 5);
}

static void test_wrap_break_at_space(void)
{
    /* "hello world" with width 10: rightmost space in [0..10] is at
     * byte 5. End = 5 (excludes the space), resume = 6 (skips it).
     * Row content is "hello"; next row starts at "world". */
    size_t resume = 0;
    size_t end = wrap_break_pos("hello world", 11, 10, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 6);
}

static void test_wrap_break_at_boundary(void)
{
    /* A space sitting exactly at position max_cells is a valid break:
     * "hello world" with width 5 — s[5] is the space — yields row
     * "hello" with the boundary space consumed. Without this rule the
     * helper would hard-break "hello" mid-word and the leading space
     * would leak onto the next row. */
    size_t resume = 0;
    size_t end = wrap_break_pos("hello world", 11, 5, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 6);
}

static void test_wrap_break_hard_split(void)
{
    /* No space anywhere in [0..max_cells]: hard-break at the column
     * boundary. Use a fixture where the only space sits well past the
     * window. */
    size_t resume = 0;
    size_t end = wrap_break_pos("helloworldmore stuff", 20, 10, &resume);
    EXPECT(end == 10);
    EXPECT(resume == 10);
}

static void test_wrap_break_trims_trailing_spaces(void)
{
    /* Defensive trim: if the input wasn't pre-flattened, a run of
     * spaces before the break point shouldn't leak into the row.
     * Input "hi  more" with width 5: the rightmost space in [0..5] is
     * at byte 3; the trim then walks end back over the space at byte
     * 2, landing end=2 (row "hi") with resume=4 (start of "more"). */
    size_t resume = 0;
    size_t end = wrap_break_pos("hi  more", 8, 5, &resume);
    EXPECT(end == 2);
    EXPECT(resume == 4);
}

static void test_wrap_break_null_resume(void)
{
    /* resume_at NULL is allowed for callers that don't need it. */
    size_t end = wrap_break_pos("hello world", 11, 10, NULL);
    EXPECT(end == 5);
}

static void test_wrap_break_multibyte_aware(void)
{
    /* "café world" — 10 cells, 11 bytes (é is 2 bytes, 1 cell).
     * With max_cells=4, the boundary space at column 4 is byte 5
     * (since é occupies bytes 3-4). Row content "café" (4 cells,
     * 5 bytes); resume at "world" (byte 6). A byte-counting
     * implementation would have broken mid-é. */
    size_t resume = 0;
    size_t end = wrap_break_pos("caf\xC3\xA9 world", 11, 4, &resume);
    EXPECT(end == 5);
    EXPECT(resume == 6);
}

static void test_wrap_break_keeps_combining_with_base(void)
{
    /* "abcd̃ef" — a, b, c, d+COMBINING TILDE, e, f. 6 cells, 8 bytes
     * (combining tilde U+0303 = CC 83). With max_cells=4 and no space,
     * the hard split should land *after* the combining mark so it
     * stays attached to its base 'd', not orphaned at the start of
     * the next row. */
    size_t resume = 0;
    size_t end = wrap_break_pos("abcd\xCC\x83"
                                "ef",
                                8, 4, &resume);
    EXPECT(end == 6);    /* "abcd̃" = 4 ASCII + 2-byte combining */
    EXPECT(resume == 6); /* hard-break, no space consumed */
}

static void test_wrap_break_oversized_first_codepoint(void)
{
    /* Pathological budget: max_cells=1, first codepoint is 2 cells
     * (emoji). No space anywhere in the window. Without forward-
     * progress protection, advance_cells would return 0 and stall
     * the caller's loop. Helper takes the emoji as a single-codepoint
     * row instead — visible row overflows by 1 cell, but the caller
     * still advances and the row count stays bounded. */
    size_t resume = 0;
    /* "🦀abc" — emoji (4 bytes) + abc. */
    size_t end = wrap_break_pos("\xF0\x9F\xA6\x80"
                                "abc",
                                7, 1, &resume);
    EXPECT(end == 4);    /* one emoji codepoint = 4 bytes */
    EXPECT(resume == 4); /* hard-break, no space consumed */
}

/* ---------- reflow_for_display ---------- */

static void test_reflow_no_wrap_needed(void)
{
    /* Fits on first row with reserve room: pass-through dup. */
    char *out = reflow_for_display("short", 80, 80, 3, 0);
    EXPECT_STR_EQ(out, "short");
    free(out);
}

static void test_reflow_wraps_at_word(void)
{
    /* Two rows of width 10. Input "hello world more" (16 chars):
     *   row 0 (cap 10): "hello"      ← break at space, "world more" remains
     *   row 1 (cap 10): "world more" ← fits exactly
     * No truncation. */
    char *out = reflow_for_display("hello world more", 10, 10, 2, 0);
    EXPECT_STR_EQ(out, "hello\nworld more");
    free(out);
}

static void test_reflow_truncates_when_out_of_rows(void)
{
    /* Two rows of width 10, input "hello world more here". After row 0
     * ("hello"), 15 chars remain ("world more here"); row 1 is the last
     * row so we reserve 3 for "..." and word-break in width-3=7. Window
     * "world m" — last space at byte 5, so row 1 = "world" + "...". */
    char *out = reflow_for_display("hello world more here", 10, 10, 2, 0);
    EXPECT_STR_EQ(out, "hello\nworld...");
    free(out);
}

static void test_reflow_long_unbreakable_word(void)
{
    /* No spaces in the input but still longer than one row: hard-break
     * mid-word, then truncate the last row. width=10, max_rows=2 →
     * row 0 = first 10 chars, row 1 = next 7 + "...". */
    char *out = reflow_for_display("abcdefghijklmnopqrstuvwxyz", 10, 10, 2, 0);
    EXPECT_STR_EQ(out, "abcdefghij\nklmnopq...");
    free(out);
}

static void test_reflow_unbreakable_across_3_rows_truncates(void)
{
    /* The pathological case: a long unbroken token that overflows even
     * a 3-row budget. Every row hard-breaks at the column boundary
     * (no space anywhere in the input), and the last row hard-cuts
     * at width-3 to leave room for the trailing "...". width=10,
     * max_rows=3, input 40 a's → rows 0 and 1 each take 10 chars,
     * row 2 takes 7 chars + "..." (10 cells visible). */
    char input[41];
    memset(input, 'a', 40);
    input[40] = '\0';
    char *out = reflow_for_display(input, 10, 10, 3, 0);
    EXPECT_STR_EQ(out, "aaaaaaaaaa\naaaaaaaaaa\naaaaaaa...");
    free(out);
}

static void test_reflow_unbreakable_fits_in_3_rows(void)
{
    /* Same shape but shorter — fits exactly in 3 rows of 10, no
     * truncation. Boundary case: 30 a's exactly fills the budget. */
    char input[31];
    memset(input, 'a', 30);
    input[30] = '\0';
    char *out = reflow_for_display(input, 10, 10, 3, 0);
    EXPECT_STR_EQ(out, "aaaaaaaaaa\naaaaaaaaaa\naaaaaaaaaa");
    free(out);
}

static void test_reflow_first_row_smaller(void)
{
    /* First row narrower than mid (caller has prefix). first_row=5,
     * mid_row=10, max_rows=2. "hello brave new world" (21):
     *   row 0 (cap 5):  "hello"        ← break at space at byte 5
     *   row 1 (cap 10, last): 15 chars "brave new world" remain; word-
     *     break in width-3=7 → window "brave n", last space at byte 5,
     *     row = "brave..." */
    char *out = reflow_for_display("hello brave new world", 5, 10, 2, 0);
    EXPECT_STR_EQ(out, "hello\nbrave...");
    free(out);
}

static void test_reflow_last_row_strict_for_wide_codepoint(void)
{
    /* The ellipsis must not force a wide codepoint beyond the row budget. */
    char *out = reflow_for_display("\xE7\x95\x8C"
                                   "xxx",
                                   4, 4, 1, 0);
    EXPECT_STR_EQ(out, "...");
    free(out);
}

static void test_reflow_reserve_applies_when_tail_fits_early(void)
{
    /* Every row leaves suffix space because it may become the last emitted row. */
    char *out = reflow_for_display("abcdef", 10, 10, 3, 5);
    EXPECT_STR_EQ(out, "abcde\nf");
    free(out);
}

static void test_reflow_last_row_reserve(void)
{
    /* last_row_reserve shrinks the LAST row's effective width so a
     * caller-appended suffix fits. Single-row mode (max_rows=1):
     * input "hello world" (11), first_row=11, reserve=4 → effective
     * width 7, doesn't fit → truncate. Width-3=4, window "hell", no
     * space → hard cut → "hell...". */
    char *out = reflow_for_display("hello world", 11, 11, 1, 4);
    EXPECT_STR_EQ(out, "hell...");
    free(out);
}

static void test_reflow_null_input(void)
{
    char *out = reflow_for_display(NULL, 80, 80, 3, 0);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_reflow_empty_input(void)
{
    char *out = reflow_for_display("", 80, 80, 3, 0);
    EXPECT_STR_EQ(out, "");
    free(out);
}

static void test_reflow_long_bash_command(void)
{
    const char *cmd =
        "find . -type f -name '*.c' -not -path './build/*' -not -path './build-asan/*' "
        "| xargs grep -l 'TODO' "
        "| head -20 "
        "| while read f; do echo \"== $f ==\"; grep -n TODO \"$f\"; done";

    char *out = reflow_for_display(cmd, 93, 100, 3, 0);

    /* Assert layout constraints rather than coupling this fixture to exact word breaks. */
    int rows = 1;
    for (const char *p = out; *p; p++)
        if (*p == '\n')
            rows++;
    EXPECT(rows <= 3);

    /* Each row fits its budget. */
    int row = 0;
    int row_len = 0;
    for (const char *p = out;; p++) {
        if (*p == '\n' || *p == '\0') {
            int budget = (row == 0) ? 93 : 100;
            if (row_len > budget)
                FAIL("row %d length %d exceeds budget %d", row, row_len, budget);
            if (*p == '\0')
                break;
            row++;
            row_len = 0;
        } else {
            row_len++;
        }
    }

    /* Row 0 starts with "find ", confirms the head of the input is
     * preserved verbatim (no stray truncation up front). */
    EXPECT(strncmp(out, "find . -type f", 14) == 0);
    free(out);
}

static void test_reflow_long_bash_command_truncated(void)
{
    const char *cmd = "find . -type f -name '*.c' -not -path './build/*' "
                      "| xargs grep -l TODO | head | while read f; do echo $f; done";

    char *out = reflow_for_display(cmd, 33, 40, 2, 0);

    /* Exactly 2 rows. */
    int newlines = 0;
    for (const char *p = out; *p; p++)
        if (*p == '\n')
            newlines++;
    EXPECT(newlines == 1);

    /* Last row ends with "..." since the input doesn't fit in 2 rows. */
    size_t n = strlen(out);
    EXPECT(n >= 3);
    EXPECT(memcmp(out + n - 3, "...", 3) == 0);

    /* Row 0 starts with the head of the command. */
    EXPECT(strncmp(out, "find . -type f", 14) == 0);
    free(out);
}

/* ---------- parse_int / display_width ---------- */

static void test_parse_int(void)
{
    int value = 0;
    EXPECT(parse_int("42", &value));
    EXPECT(value == 42);

    char text[64];
    snprintf(text, sizeof(text), "%d", INT_MIN);
    EXPECT(parse_int(text, &value));
    EXPECT(value == INT_MIN);
    snprintf(text, sizeof(text), "%d", INT_MAX);
    EXPECT(parse_int(text, &value));
    EXPECT(value == INT_MAX);

    value = 7;
    EXPECT(!parse_int(NULL, &value));
    EXPECT(!parse_int("", &value));
    EXPECT(!parse_int("12x", &value));
    EXPECT(!parse_int("999999999999999999999", &value));
    EXPECT(value == 7);
}

static void test_display_width_capped(void)
{
    unsetenv("HAX_DISPLAY_WIDTH");
    int expected = term_width();
    if (expected > DISPLAY_WIDTH_CAP)
        expected = DISPLAY_WIDTH_CAP;
    if (expected < 20)
        expected = 20;
    EXPECT(display_width() == expected);

    setenv("HAX_DISPLAY_WIDTH", "auto", 1);
    EXPECT(display_width() == expected);
    unsetenv("HAX_DISPLAY_WIDTH");
}

static void test_display_width_env_override(void)
{
    int terminal = term_width();
    if (terminal < 20)
        terminal = 20;
    setenv("HAX_DISPLAY_WIDTH", "terminal", 1);
    EXPECT(display_width() == terminal);
    setenv("HAX_DISPLAY_WIDTH", "TERMINAL", 1);
    EXPECT(display_width() == terminal);

    /* An exact width bypasses both terminal detection and the soft cap. */
    setenv("HAX_DISPLAY_WIDTH", "120", 1);
    EXPECT(display_width() == 120);
    setenv("HAX_DISPLAY_WIDTH", "60", 1);
    EXPECT(display_width() == 60);
    setenv("HAX_DISPLAY_WIDTH", "500", 1);
    EXPECT(display_width() == 500);

    int automatic = term_width();
    if (automatic > DISPLAY_WIDTH_CAP)
        automatic = DISPLAY_WIDTH_CAP;
    if (automatic < 20)
        automatic = 20;
    /* Out-of-range, malformed, and overflowing values fall back to auto. */
    setenv("HAX_DISPLAY_WIDTH", "5", 1);
    EXPECT(display_width() == automatic);
    setenv("HAX_DISPLAY_WIDTH", "abc", 1);
    EXPECT(display_width() == automatic);
    setenv("HAX_DISPLAY_WIDTH", "80x", 1);
    EXPECT(display_width() == automatic);
    setenv("HAX_DISPLAY_WIDTH", "999999999999999999999999999", 1);
    EXPECT(display_width() == automatic);
    unsetenv("HAX_DISPLAY_WIDTH");
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

/* ---------- format_tokens / format_duration / format_cost ---------- */

static void test_format_tokens_ranges(void)
{
    char buf[32];
    format_tokens(buf, sizeof(buf), -1);
    EXPECT_STR_EQ(buf, "?");
    format_tokens(buf, sizeof(buf), 412);
    EXPECT_STR_EQ(buf, "412");
    format_tokens(buf, sizeof(buf), 5 * 1024 + 410); /* 5.4k */
    EXPECT_STR_EQ(buf, "5.4k");
    format_tokens(buf, sizeof(buf), 128L * 1024);
    EXPECT_STR_EQ(buf, "128k");
    format_tokens(buf, sizeof(buf), 1228L * 1024); /* ~1.2M */
    EXPECT_STR_EQ(buf, "1.2M");
    format_tokens(buf, sizeof(buf), 12L * 1024 * 1024);
    EXPECT_STR_EQ(buf, "12M");
}

static void test_format_duration_ranges(void)
{
    char buf[32];
    format_duration(buf, sizeof(buf), 0);
    EXPECT_STR_EQ(buf, "0s");
    format_duration(buf, sizeof(buf), -5); /* clamps, never "-0s" */
    EXPECT_STR_EQ(buf, "0s");
    format_duration(buf, sizeof(buf), 42499); /* rounds down */
    EXPECT_STR_EQ(buf, "42s");
    format_duration(buf, sizeof(buf), 42500); /* rounds up */
    EXPECT_STR_EQ(buf, "43s");
    format_duration(buf, sizeof(buf), 68000);
    EXPECT_STR_EQ(buf, "1m 08s");
    format_duration(buf, sizeof(buf), 3720000);
    EXPECT_STR_EQ(buf, "1h 02m");
    /* Zero remainders are omitted: whole minutes and hours read bare. */
    format_duration(buf, sizeof(buf), 600000);
    EXPECT_STR_EQ(buf, "10m");
    format_duration(buf, sizeof(buf), 7200000);
    EXPECT_STR_EQ(buf, "2h");
    /* The steady variant keeps them, so ticking displays never shrink. */
    format_duration_steady(buf, sizeof(buf), 600000);
    EXPECT_STR_EQ(buf, "10m 00s");
    format_duration_steady(buf, sizeof(buf), 7200000);
    EXPECT_STR_EQ(buf, "2h 00m");
    format_duration_steady(buf, sizeof(buf), 68000);
    EXPECT_STR_EQ(buf, "1m 08s");
}

static void test_format_context_with_and_without_limit(void)
{
    char buf[64];
    format_context(buf, sizeof(buf), 9113, 262144); /* 8.9k / 256k, 3% */
    EXPECT_STR_EQ(buf, "8.9k / 256k (3%)");
    format_context(buf, sizeof(buf), 9113, 0); /* unknown window */
    EXPECT_STR_EQ(buf, "8.9k");
    format_context(buf, sizeof(buf), 300000, 262144); /* stale window metadata reports over 100% */
    EXPECT_STR_EQ(buf, "293k / 256k (114%)");
}

static void test_format_cost_precision(void)
{
    char buf[32];
    format_cost(buf, sizeof(buf), 0.0);
    EXPECT_STR_EQ(buf, "$0.00");
    format_cost(buf, sizeof(buf), 0.00421);
    EXPECT_STR_EQ(buf, "$0.0042");
    format_cost(buf, sizeof(buf), 0.042);
    EXPECT_STR_EQ(buf, "$0.042");
    format_cost(buf, sizeof(buf), 1.234);
    EXPECT_STR_EQ(buf, "$1.23");
    format_cost(buf, sizeof(buf), 42.129);
    EXPECT_STR_EQ(buf, "$42.13");
}

static void test_format_extreme_values(void)
{
    char formatted[64];
    format_tokens(formatted, sizeof(formatted), LONG_MAX);
    EXPECT(formatted[0] != '-');

    format_duration(formatted, sizeof(formatted), LONG_MAX);
    EXPECT(formatted[0] != '-');
    EXPECT(strchr(formatted, 'h') != NULL);

    format_context(formatted, sizeof(formatted), LONG_MAX, 1);
    EXPECT(strstr(formatted, "(999%)") != NULL);
}

static void test_shell_single_quote(void)
{
    char *q = shell_single_quote("plain");
    EXPECT_STR_EQ(q, "'plain'");
    free(q);

    q = shell_single_quote("it's");
    EXPECT_STR_EQ(q, "'it'\\''s'");
    free(q);

    /* metacharacters are inert inside single quotes — no escaping */
    q = shell_single_quote("a b;$(x)|&\"*");
    EXPECT_STR_EQ(q, "'a b;$(x)|&\"*'");
    free(q);

    q = shell_single_quote("");
    EXPECT_STR_EQ(q, "''");
    free(q);

    q = shell_single_quote(NULL);
    EXPECT_STR_EQ(q, "''");
    free(q);
}

int main(void)
{
    /* truncate_for_display / wrap_break_pos / reflow_for_display use
     * utf8_codepoint_cells (mbrtowc + wcwidth) for cell-accurate width
     * — those need a UTF-8 LC_CTYPE. */
    locale_init_utf8();

    test_diag_sequence();

    test_uuid_v4_format();
    test_uuid_v4_unique();

    test_zero_sized_allocations();
    test_buf_append_and_steal();
    test_buf_steal_empty();
    test_buf_reset_keeps_capacity();
    test_buf_grows_repeatedly();

    test_slurp_missing();
    test_slurp_empty();
    test_slurp_normal();
    test_slurp_directory_rejected();
    test_slurp_fifo_rejected_no_hang();
    test_slurp_capped_missing();
    test_slurp_capped_under();
    test_slurp_capped_zero();
    test_slurp_capped_does_not_preallocate_cap();
    test_slurp_capped_over();
    test_slurp_capped_exact();

    test_cap_lines_no_long_lines();
    test_cap_lines_empty();
    test_cap_lines_truncates_long_line();
    test_cap_lines_preserves_short_neighbors();
    test_cap_lines_long_line_no_trailing_newline();

    test_parse_size_basic();
    test_parse_size_invalid_returns_zero();
    test_parse_size_rejects_overflow();

    test_flatten_null();
    test_flatten_empty();
    test_flatten_plain();
    test_flatten_newline();
    test_flatten_collapses_runs();
    test_flatten_strips_edges();
    test_flatten_all_whitespace();
    test_flatten_control_bytes();
    test_display_cells();
    test_flatten_preserves_high_bytes();
    test_flatten_substitutes_bidi_override();
    test_flatten_substitutes_zwj();
    test_flatten_substitutes_malformed_utf8();
    test_flatten_caps_zero_width_run();
    test_flatten_preserves_legit_combining_run();

    test_parse_duration_plain_seconds();
    test_parse_duration_with_suffix();
    test_parse_duration_whitespace();
    test_parse_duration_invalid();

    test_format_tokens_ranges();
    test_format_duration_ranges();
    test_format_cost_precision();
    test_format_extreme_values();
    test_shell_single_quote();
    test_format_context_with_and_without_limit();

    test_truncate_under_cap();
    test_truncate_exact_cap();
    test_truncate_above_cap();
    test_truncate_tiny_cap();
    test_truncate_utf8_boundary();
    test_truncate_keeps_multibyte_intact();
    test_truncate_under_cap_multibyte();
    test_truncate_keeps_trailing_combining_mark();
    test_truncate_emoji_two_cells();
    test_truncate_null();

    test_wrap_break_fits();
    test_wrap_break_at_space();
    test_wrap_break_at_boundary();
    test_wrap_break_hard_split();
    test_wrap_break_trims_trailing_spaces();
    test_wrap_break_null_resume();
    test_wrap_break_multibyte_aware();
    test_wrap_break_keeps_combining_with_base();
    test_wrap_break_oversized_first_codepoint();

    test_reflow_no_wrap_needed();
    test_reflow_wraps_at_word();
    test_reflow_truncates_when_out_of_rows();
    test_reflow_long_unbreakable_word();
    test_reflow_unbreakable_across_3_rows_truncates();
    test_reflow_unbreakable_fits_in_3_rows();
    test_reflow_first_row_smaller();
    test_reflow_last_row_strict_for_wide_codepoint();
    test_reflow_reserve_applies_when_tail_fits_early();
    test_reflow_last_row_reserve();
    test_reflow_null_input();
    test_reflow_empty_input();
    test_reflow_long_bash_command();
    test_reflow_long_bash_command_truncated();

    test_parse_int();
    test_display_width_capped();
    test_display_width_env_override();

    T_REPORT();
}
