/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

/* ---------- parse_int ---------- */

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
    format_context(buf, sizeof(buf), -1, 262144); /* known window, no usage reported yet */
    EXPECT_STR_EQ(buf, "? / 256k");
    format_context(buf, sizeof(buf), -1, 0); /* nothing known */
    EXPECT_STR_EQ(buf, "?");
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

/* C.UTF-8, C.utf8 and a bare UTF-8 are all spellings hax may settle on, so ask the C library what a
 * name means rather than matching its text. */
static int names_utf8(const char *locale)
{
    if (!locale)
        return 0;
    char *restore = xstrdup(setlocale(LC_CTYPE, NULL));
    int is_utf8 = setlocale(LC_CTYPE, locale) && strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
    setlocale(LC_CTYPE, restore);
    free(restore);
    return is_utf8;
}

/* Children read the environment, not this process's locale. OpenBSD arrives here by default. */
static void test_locale_override_reaches_the_environment(void)
{
    unsetenv("LC_ALL");
    setenv("LANG", "C", 1);
    setenv("LC_CTYPE", "C", 1);

    locale_init_utf8();
    if (!locale_have_utf8())
        T_SKIP("no UTF-8 locale available to switch to");

    EXPECT(names_utf8(getenv("LC_CTYPE")));
    /* The environment carries it, so a child needs nothing further. */
    EXPECT(locale_child_ctype_override() == NULL);
}

/* LC_ALL outranks LC_CTYPE, so an override under it would not reach children anyway, and clearing
 * it would hand every other category to LANG. This process still needs UTF-8 to measure text. */
static void test_locale_defers_to_a_deliberate_lc_all(void)
{
    setenv("LANG", "de_DE.UTF-8", 1);
    setenv("LC_CTYPE", "C", 1);
    setenv("LC_ALL", "C", 1);

    locale_init_utf8();
    if (!locale_have_utf8())
        T_SKIP("no UTF-8 locale available to switch to");

    EXPECT_STR_EQ(getenv("LC_ALL"), "C");
    EXPECT_STR_EQ(getenv("LC_CTYPE"), "C");
    /* Nothing in the environment will decode UTF-8, so a child has to be handed a locale. */
    EXPECT(names_utf8(locale_child_ctype_override()));
}

/* A deliberate LC_ALL survives, because the other categories under it were never in question. */
static void test_locale_leaves_a_utf8_environment_alone(const char *utf8_locale)
{
    setenv("LC_ALL", utf8_locale, 1);
    setenv("LC_CTYPE", utf8_locale, 1);

    locale_init_utf8();

    EXPECT_STR_EQ(getenv("LC_ALL"), utf8_locale);
}

int main(void)
{
    test_locale_override_reaches_the_environment();
    /* Whichever name the override proved available; the two are not both present everywhere. Copied
     * because setenv() may reallocate the block the value points into. */
    char *utf8_locale = xstrdup(getenv("LC_CTYPE"));
    if (names_utf8(utf8_locale))
        test_locale_leaves_a_utf8_environment_alone(utf8_locale);
    free(utf8_locale);
    test_locale_defers_to_a_deliberate_lc_all();

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

    test_parse_size_basic();
    test_parse_size_invalid_returns_zero();
    test_parse_size_rejects_overflow();

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

    test_parse_int();

    T_REPORT();
}
