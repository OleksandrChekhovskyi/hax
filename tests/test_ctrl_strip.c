/* SPDX-License-Identifier: MIT */
#include "ctrl_strip.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"

/* Sanitize via the chunked API in one shot. Caller frees. */
static char *strip_one_shot(const char *in)
{
    size_t n = strlen(in);
    char *out = malloc(n + 1);
    struct ctrl_strip s;
    ctrl_strip_init(&s);
    size_t w = ctrl_strip_feed(&s, in, n, out);
    out[w] = '\0';
    return out;
}

/* Feed an explicit-length buffer (handles embedded NULs). Caller frees;
 * *out_len receives the number of bytes written (no NUL terminator). */
static char *strip_bytes(const char *in, size_t n, size_t *out_len)
{
    char *out = malloc(n + 1);
    struct ctrl_strip s;
    ctrl_strip_init(&s);
    size_t w = ctrl_strip_feed(&s, in, n, out);
    out[w] = '\0';
    *out_len = w;
    return out;
}

/* Sanitize feeding one byte at a time so any state-machine boundary bug
 * surfaces. Caller frees. */
static char *strip_byte_by_byte(const char *in)
{
    size_t n = strlen(in);
    char *out = malloc(n + 1);
    struct ctrl_strip s;
    ctrl_strip_init(&s);
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        w += ctrl_strip_feed(&s, in + i, 1, out + w);
    out[w] = '\0';
    return out;
}

static void test_passthrough(void)
{
    char *got = strip_one_shot("hello world\n\ttabbed\n");
    EXPECT_STR_EQ(got, "hello world\n\ttabbed\n");
    free(got);
}

static void test_csi_sgr(void)
{
    char *got = strip_one_shot("\x1b[31mred\x1b[0m and \x1b[1;33mbold-yellow\x1b[m");
    EXPECT_STR_EQ(got, "red and bold-yellow");
    free(got);
}

static void test_csi_cursor(void)
{
    /* Cursor moves and erase-line: all CSI with various finals. */
    char *got = strip_one_shot("\x1b[2J\x1b[H\x1b[10;20Hhi");
    EXPECT_STR_EQ(got, "hi");
    free(got);
}

static void test_osc_bel(void)
{
    char *got = strip_one_shot("before\x1b]0;window title\x07"
                               "after");
    EXPECT_STR_EQ(got, "beforeafter");
    free(got);
}

static void test_osc_st(void)
{
    /* ESC \ terminator (String Terminator). */
    char *got = strip_one_shot("a\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\b");
    EXPECT_STR_EQ(got, "alinkb");
    free(got);
}

static void test_dcs(void)
{
    char *got = strip_one_shot("x\x1bP1;2;3qstuff\x1b\\y");
    EXPECT_STR_EQ(got, "xy");
    free(got);
}

static void test_single_byte_esc(void)
{
    /* ESC c (full reset), ESC = / ESC > (keypad). */
    char *got = strip_one_shot("a\x1b"
                               "cb\x1b=c\x1b>d");
    EXPECT_STR_EQ(got, "abcd");
    free(got);
}

static void test_intermediate_esc(void)
{
    /* ESC ( B — designate G0 charset as US-ASCII. */
    char *got = strip_one_shot("a\x1b(Bb\x1b)0c");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_bare_cr_dropped(void)
{
    char *got = strip_one_shot("loading...\rdone\n");
    EXPECT_STR_EQ(got, "loading...done\n");
    free(got);
}

static void test_crlf_preserved_as_lf(void)
{
    char *got = strip_one_shot("line1\r\nline2\r\n");
    EXPECT_STR_EQ(got, "line1\nline2\n");
    free(got);
}

static void test_backspace_and_bell_dropped(void)
{
    char *got = strip_one_shot("a\bb\ac");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_form_feed_and_vt_dropped(void)
{
    /* FF (\f, 0x0C) clears the screen on some terminals; VT (\v, 0x0B)
     * jumps the cursor. Both must not survive into the dim block. */
    char *got = strip_one_shot("a\fb\vc");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_so_si_dropped(void)
{
    /* SO (0x0E) / SI (0x0F) shift to/from alternate charset. Leaving
     * either to the terminal can wedge subsequent text into line-drawing
     * glyphs until a reset. */
    char *got = strip_one_shot("a\x0e"
                               "b\x0f"
                               "c");
    EXPECT_STR_EQ(got, "abc");
    free(got);
}

static void test_misc_c0_dropped(void)
{
    /* Sample of the rest of C0: SOH, ACK, DLE, CAN, SUB, FS, US. */
    char *got = strip_one_shot("a\x01"
                               "b\x06"
                               "c\x10"
                               "d\x18"
                               "e\x1a"
                               "f\x1c"
                               "g\x1f"
                               "h");
    EXPECT_STR_EQ(got, "abcdefgh");
    free(got);
}

static void test_del_dropped(void)
{
    char *got = strip_one_shot("a\x7f"
                               "b");
    EXPECT_STR_EQ(got, "ab");
    free(got);
}

static void test_embedded_nul(void)
{
    /* NUL embedded in the input is dropped by the explicit-length feed
     * path. (NUL-terminated input bounds itself, so ctrl_strip_dup
     * doesn't see beyond the first NUL anyway.) */
    const char in[] = {'a', 0x00, 'b', 0x00, 'c'};
    size_t out_len = 0;
    char *got = strip_bytes(in, sizeof(in), &out_len);
    EXPECT_MEM_EQ(got, out_len, "abc", 3);
    free(got);
}

static void test_high_bytes_preserved(void)
{
    /* 8-bit C1 controls (0x80-0x9F) and the rest of the high half are
     * left alone — in a UTF-8 stream they're continuation bytes. U+2713
     * CHECK MARK is E2 9C 93. */
    char *got = strip_one_shot("\x1b[32m\xe2\x9c\x93\x1b[0m ok");
    EXPECT_STR_EQ(got, "\xe2\x9c\x93 ok");
    free(got);
}

static void test_chunk_boundary(void)
{
    /* The same input fed byte-by-byte must produce the same output as
     * one-shot, even when the split lands inside an escape sequence.
     * Each input is chosen to traverse a different state at a byte
     * boundary; together they cover every absorbing state in the
     * machine (S_CSI, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC, S_ESC_INTER). */
    static const struct {
        const char *in;
        const char *want;
    } cases[] = {
        /* CSI + OSC (BEL term) + intermediate ESC + FF dropped in S_NORMAL. */
        {"pre\x1b[31mRED\x1b[0m\x1b]0;t\x07mid\x1b(B\fend\n", "preREDmidend\n"},
        /* OSC with ST terminator — exercises S_OSC_ESC at every offset. */
        {"a\x1b]8;;u\x1b\\b", "ab"},
        /* DCS with ST terminator — exercises S_STR + S_STR_ESC. */
        {"a\x1bP1;2;3qstuff\x1b\\b", "ab"},
        /* Bare intermediate ESC sequence — exercises S_ESC_INTER. */
        {"a\x1b(Bb\x1b)0c", "abc"},
        /* Aborts: LF inside CSI / CAN inside OSC / SUB inside DCS. */
        {"a\x1b[\nb", "a\nb"},
        {"a\x1b]title\x18"
         "b",
         "ab"},
        {"a\x1bPdcs\x1a"
         "b",
         "ab"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *a = strip_one_shot(cases[i].in);
        char *b = strip_byte_by_byte(cases[i].in);
        EXPECT_STR_EQ(a, b);
        EXPECT_STR_EQ(a, cases[i].want);
        free(a);
        free(b);
    }
}

static void test_chunk_split_at_malformed_esc(void)
{
    /* A malformed-ESC reconsume that lands at offset 0 of a feed call
     * exercises the size_t underflow-then-restore in the i-- path. The
     * first feed leaves state = S_ESC; the second feed sees a bad next
     * byte at i=0, must drop the ESC and reconsume the byte under
     * S_NORMAL (where 0x01 is dropped) without skipping the following
     * 'X'. */
    char buf[8];
    struct ctrl_strip s;
    ctrl_strip_init(&s);
    size_t w = 0;
    w += ctrl_strip_feed(&s, "\x1b", 1, buf + w);
    w += ctrl_strip_feed(&s, "\x01X", 2, buf + w);
    buf[w] = '\0';
    EXPECT_STR_EQ(buf, "X");
}

static void test_abort_in_csi(void)
{
    /* Without the LF-abort, "\x1b[\n\n\nfoo\n" would absorb everything
     * up to 'f' (the next byte in 0x40-0x7E) and emit "oo\n" — three
     * newlines and the leading character would be silently eaten. */
    char *got = strip_one_shot("\x1b[\n\n\nimportant message\n");
    EXPECT_STR_EQ(got, "\n\n\nimportant message\n");
    free(got);
}

static void test_abort_in_osc(void)
{
    /* CAN (0x18) cancels an OSC. CAN itself is a C0 control and is
     * dropped by S_NORMAL, so the recovered text starts at 'r'. */
    char *got = strip_one_shot("\x1b]2;title\x18rest\n");
    EXPECT_STR_EQ(got, "rest\n");
    free(got);
}

static void test_abort_in_dcs(void)
{
    /* SUB (0x1A) cancels a DCS. */
    char *got = strip_one_shot("\x1bP1;2;3qpayload\x1arest\n");
    EXPECT_STR_EQ(got, "rest\n");
    free(got);
}

static void test_partial_escape_at_eof(void)
{
    /* Trailing partial CSI / OSC are dropped; output stays well-formed. */
    char *a = strip_one_shot("ok\x1b[3");
    EXPECT_STR_EQ(a, "ok");
    free(a);
    char *b = strip_one_shot("ok\x1b]0;tit");
    EXPECT_STR_EQ(b, "ok");
    free(b);
    char *c = strip_one_shot("ok\x1b");
    EXPECT_STR_EQ(c, "ok");
    free(c);
}

static void test_dup_helper(void)
{
    char *got = ctrl_strip_dup("\x1b[1mhi\x1b[0m\f\n");
    EXPECT_STR_EQ(got, "hi\n");
    free(got);
}

int main(void)
{
    test_passthrough();
    test_csi_sgr();
    test_csi_cursor();
    test_osc_bel();
    test_osc_st();
    test_dcs();
    test_single_byte_esc();
    test_intermediate_esc();
    test_bare_cr_dropped();
    test_crlf_preserved_as_lf();
    test_backspace_and_bell_dropped();
    test_form_feed_and_vt_dropped();
    test_so_si_dropped();
    test_misc_c0_dropped();
    test_del_dropped();
    test_embedded_nul();
    test_high_bytes_preserved();
    test_chunk_boundary();
    test_chunk_split_at_malformed_esc();
    test_abort_in_csi();
    test_abort_in_osc();
    test_abort_in_dcs();
    test_partial_escape_at_eof();
    test_dup_helper();
    T_REPORT();
}
