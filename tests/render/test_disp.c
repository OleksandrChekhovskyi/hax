/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "render/disp.h"

/* disp_* writes to stdout; tests redirect stdout to a tmp file via
 * freopen and read it back. The file is unlinked after open so it's
 * cleaned up on process exit, and ftruncate+rewind reset between tests
 * without re-opening. */

static char captured[65536];

static void cap_init(void)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/haxdisp.%d.out", (int)getpid());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        exit(1);
    }
    unlink(path);
}

static void cap_reset(void)
{
    fflush(stdout);
    if (ftruncate(fileno(stdout), 0) != 0) {
        perror("ftruncate");
        exit(1);
    }
    rewind(stdout);
}

static const char *cap_read(void)
{
    fflush(stdout);
    fseek(stdout, 0, SEEK_SET);
    size_t n = fread(captured, 1, sizeof(captured) - 1, stdout);
    captured[n] = 0;
    return captured;
}

/* ---------- putc / write defer trailing newlines ---------- */

static void test_putc_commits_chars(void)
{
    cap_reset();
    struct disp d = {0};
    disp_putc(&d, 'h');
    disp_putc(&d, 'i');
    EXPECT_STR_EQ(cap_read(), "hi");
    EXPECT(d.trail == 0);
    EXPECT(d.held == 0);
}

static void test_putc_holds_trailing_newlines(void)
{
    cap_reset();
    struct disp d = {0};
    disp_putc(&d, 'a');
    disp_putc(&d, '\n');
    disp_putc(&d, '\n');
    /* Newlines were buffered, only 'a' on stdout so far. */
    EXPECT_STR_EQ(cap_read(), "a");
    EXPECT(d.held == 2);
    /* emit_held drains them. */
    cap_reset();
    disp_emit_held(&d);
    EXPECT_STR_EQ(cap_read(), "\n\n");
    EXPECT(d.held == 0);
    EXPECT(d.trail == 2);
}

static void test_putc_non_nl_after_held_commits(void)
{
    cap_reset();
    struct disp d = {0};
    disp_putc(&d, 'a');
    disp_putc(&d, '\n');
    disp_putc(&d, 'b');
    /* 'b' commits the held \n before writing itself. */
    EXPECT_STR_EQ(cap_read(), "a\nb");
}

static void test_write_holds_trailing_newlines(void)
{
    cap_reset();
    struct disp d = {0};
    disp_write(&d, "hi\n\n", 4);
    EXPECT_STR_EQ(cap_read(), "hi");
    EXPECT(d.held == 2);
}

static void test_write_strips_crlf_tail(void)
{
    cap_reset();
    struct disp d = {0};
    /* CRLF tail is fully deferred; only \n increments held, the \r is
     * absorbed and never emitted. */
    disp_write(&d, "hi\r\n", 4);
    EXPECT_STR_EQ(cap_read(), "hi");
    EXPECT(d.held == 1);
    cap_reset();
    disp_emit_held(&d);
    EXPECT_STR_EQ(cap_read(), "\n");
}

static void test_write_empty_is_noop(void)
{
    cap_reset();
    struct disp d = {0};
    disp_write(&d, "", 0);
    EXPECT_STR_EQ(cap_read(), "");
    EXPECT(d.held == 0);
    EXPECT(d.trail == 0);
}

/* ---------- block separator caps held newlines ---------- */

static void test_block_separator_from_clean_state(void)
{
    cap_reset();
    struct disp d = {0};
    /* trail=0, held=0 → emit two newlines. */
    disp_block_separator(&d);
    EXPECT_STR_EQ(cap_read(), "\n\n");
    EXPECT(d.trail == 2);
    EXPECT(d.held == 0);
}

static void test_block_separator_drops_excess_held(void)
{
    cap_reset();
    struct disp d = {0};
    for (int i = 0; i < 5; i++)
        disp_putc(&d, '\n');
    /* held=5; separator commits 2 (need = 2 - trail), drops the rest. */
    disp_block_separator(&d);
    EXPECT_STR_EQ(cap_read(), "\n\n");
    EXPECT(d.held == 0);
    EXPECT(d.trail == 2);
}

static void test_block_separator_preserves_committed_trail(void)
{
    cap_reset();
    struct disp d = {.trail = 2};
    /* Already at one blank line — separator emits nothing. */
    disp_block_separator(&d);
    EXPECT_STR_EQ(cap_read(), "");
    EXPECT(d.trail == 2);
}

/* ---------- printf goes through write ---------- */

static void test_printf_basic(void)
{
    cap_reset();
    struct disp d = {0};
    disp_printf(&d, "n=%d s=%s", 42, "hi");
    EXPECT_STR_EQ(cap_read(), "n=42 s=hi");
}

static void test_printf_holds_trailing_newline(void)
{
    cap_reset();
    struct disp d = {0};
    disp_printf(&d, "%s\n", "row");
    EXPECT_STR_EQ(cap_read(), "row");
    EXPECT(d.held == 1);
}

/* ---------- first_delta_strip drops leading newlines only ---------- */

static void test_first_delta_strip_drops_leading_newlines(void)
{
    struct disp d = {.saw_text = 0};
    const char *s = "\n\n\thello";
    size_t n = strlen(s);
    disp_first_delta_strip(&d, &s, &n);
    EXPECT(n == 6);
    EXPECT(strncmp(s, "\thello", 6) == 0);
}

static void test_first_delta_strip_drops_cr_too(void)
{
    struct disp d = {.saw_text = 0};
    const char *s = "\r\n\rhello";
    size_t n = strlen(s);
    disp_first_delta_strip(&d, &s, &n);
    EXPECT(n == 5);
    EXPECT(strncmp(s, "hello", 5) == 0);
}

static void test_first_delta_strip_noop_when_saw_text(void)
{
    struct disp d = {.saw_text = 1};
    const char *orig = "\n\nhello";
    const char *s = orig;
    size_t n = strlen(s);
    disp_first_delta_strip(&d, &s, &n);
    EXPECT(s == orig);
    EXPECT(n == 7);
}

/* ---------- the output sink ---------- */

/* An explicit sink takes every kind of write — content, escapes, held
 * newlines, separators — and stdout sees none of it. This is what lets the
 * live pipeline render into a pager or a memory stream. */
static void test_explicit_sink_takes_all_writes(void)
{
    cap_reset();
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    EXPECT(mem != NULL);
    if (!mem)
        return;

    struct disp d = {.out = mem, .trail = 2};
    disp_raw(&d, "\x1b[1m");
    disp_printf(&d, "body");
    disp_putc(&d, '\n');
    disp_emit_held(&d);
    disp_block_separator(&d);
    disp_flush(&d);
    fclose(mem);

    EXPECT_STR_EQ(buf, "\x1b[1mbody\n\n");
    EXPECT_STR_EQ(cap_read(), ""); /* nothing leaked to stdout */
    free(buf);
}

/* A zero-initialized disp writes to stdout. Load-bearing: callers that
 * never touch `out` (the live REPL, tests constructing `struct disp {0}`)
 * depend on NULL meaning stdout, so this can't become "must be set". */
static void test_null_sink_means_stdout(void)
{
    cap_reset();
    struct disp d = {0};
    EXPECT(disp_sink(&d) == stdout);
    disp_write(&d, "to stdout", 9);
    EXPECT_STR_EQ(cap_read(), "to stdout");
}

/* Held/trail bookkeeping lives on the disp, not on the sink, so two disps
 * over different sinks don't interfere. */
static void test_sinks_keep_separate_bookkeeping(void)
{
    cap_reset();
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    EXPECT(mem != NULL);
    if (!mem)
        return;

    struct disp to_mem = {.out = mem};
    struct disp to_out = {0};
    disp_write(&to_mem, "a\n", 2); /* newline held, not committed */
    disp_write(&to_out, "b", 1);
    EXPECT(to_mem.held == 1 && to_mem.trail == 0);
    EXPECT(to_out.held == 0 && to_out.trail == 0);
    disp_emit_held(&to_mem);
    disp_flush(&to_mem);
    fclose(mem);

    EXPECT_STR_EQ(buf, "a\n");
    EXPECT_STR_EQ(cap_read(), "b");
    free(buf);
}

int main(void)
{
    cap_init();

    test_putc_commits_chars();
    test_putc_holds_trailing_newlines();
    test_putc_non_nl_after_held_commits();
    test_write_holds_trailing_newlines();
    test_write_strips_crlf_tail();
    test_write_empty_is_noop();
    test_block_separator_from_clean_state();
    test_block_separator_drops_excess_held();
    test_block_separator_preserves_committed_trail();
    test_printf_basic();
    test_printf_holds_trailing_newline();
    test_first_delta_strip_drops_leading_newlines();
    test_first_delta_strip_drops_cr_too();
    test_first_delta_strip_noop_when_saw_text();
    test_explicit_sink_takes_all_writes();
    test_null_sink_means_stdout();
    test_sinks_keep_separate_bookkeeping();

    T_REPORT();
}
