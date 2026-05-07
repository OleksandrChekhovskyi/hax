/* SPDX-License-Identifier: MIT */
#include "clipboard.h"

#include <stdlib.h>
#include <string.h>

#include "harness.h"

/* The pure sequence builder is what we can unit-test without driving
 * real clipboard I/O. The wrapper that picks between native helpers
 * and OSC 52 is exercised by hand at the REPL. */

static void test_osc52_basic(void)
{
    /* "hello" → base64 "aGVsbG8=" wrapped in ESC]52;c;<b64>BEL. */
    size_t n = 0;
    char *seq = clipboard_osc52_sequence("hello", 5, 0, &n);
    EXPECT(seq != NULL);
    const char *want = "\x1b]52;c;aGVsbG8=\x07";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);
}

static void test_osc52_empty(void)
{
    /* Zero-byte payload still produces a valid (empty-body) sequence —
     * downstream callers gate on "is there anything to copy?", not the
     * sequence builder. */
    size_t n = 0;
    char *seq = clipboard_osc52_sequence("", 0, 0, &n);
    EXPECT(seq != NULL);
    const char *want = "\x1b]52;c;\x07";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);
}

static void test_osc52_tmux_wrap(void)
{
    /* tmux passthrough: ESC P t m u x ; <inner with each ESC doubled> ESC \.
     * The inner OSC 52 form contains exactly one ESC (the leading one),
     * which becomes ESC ESC inside the wrapper. */
    size_t n = 0;
    char *seq = clipboard_osc52_sequence("hello", 5, 1, &n);
    EXPECT(seq != NULL);
    const char *want = "\x1bPtmux;\x1b\x1b]52;c;aGVsbG8=\x07\x1b\\";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);
}

static void test_osc52_size_cap(void)
{
    /* Payloads above CLIPBOARD_OSC52_MAX_BYTES return NULL — some
     * terminals silently drop oversized OSC sequences and we'd rather
     * the caller surface "too big" than have the user wonder why
     * paste is empty. */
    size_t big = CLIPBOARD_OSC52_MAX_BYTES + 1;
    char *buf = malloc(big);
    EXPECT(buf != NULL);
    memset(buf, 'x', big);

    char *seq = clipboard_osc52_sequence(buf, big, 0, NULL);
    EXPECT(seq == NULL);

    /* Exactly at the cap is still accepted. */
    seq = clipboard_osc52_sequence(buf, CLIPBOARD_OSC52_MAX_BYTES, 0, NULL);
    EXPECT(seq != NULL);
    free(seq);
    free(buf);
}

static void test_base64_padding(void)
{
    /* Three round-trips covering all three base64 padding cases. */
    size_t n = 0;
    char *seq = clipboard_osc52_sequence("f", 1, 0, &n);
    EXPECT(seq != NULL);
    const char *want = "\x1b]52;c;Zg==\x07";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);

    seq = clipboard_osc52_sequence("fo", 2, 0, &n);
    EXPECT(seq != NULL);
    want = "\x1b]52;c;Zm8=\x07";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);

    seq = clipboard_osc52_sequence("foo", 3, 0, &n);
    EXPECT(seq != NULL);
    want = "\x1b]52;c;Zm9v\x07";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);
}

static void test_base64_handles_embedded_nul(void)
{
    /* Markdown payloads shouldn't contain NULs in practice, but the
     * sequence builder takes (text, len) and must not treat NUL as a
     * terminator — pin that with a payload that has one in the middle. */
    const char raw[] = {'a', 0x00, 'b'};
    size_t n = 0;
    char *seq = clipboard_osc52_sequence(raw, sizeof(raw), 0, &n);
    EXPECT(seq != NULL);
    /* base64({0x61, 0x00, 0x62}) = "YQBi" (no padding, 3 input bytes). */
    const char *want = "\x1b]52;c;YQBi\x07";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);
}

static void test_base64_handles_high_bytes(void)
{
    /* Non-ASCII bytes (e.g. UTF-8 continuation bytes) must be base64'd
     * verbatim — Markdown carries them all the time and we don't want
     * to corrupt anything en route to the clipboard. */
    const unsigned char raw[] = {0xc3, 0xa9, 0xe2, 0x9c, 0x93}; /* "é✓" */
    size_t n = 0;
    char *seq = clipboard_osc52_sequence((const char *)raw, sizeof(raw), 0, &n);
    EXPECT(seq != NULL);
    const char *want = "\x1b]52;c;w6ninJM=\x07";
    EXPECT(n == strlen(want));
    EXPECT(memcmp(seq, want, n) == 0);
    free(seq);
}

int main(void)
{
    test_osc52_basic();
    test_osc52_empty();
    test_osc52_tmux_wrap();
    test_osc52_size_cap();
    test_base64_padding();
    test_base64_handles_embedded_nul();
    test_base64_handles_high_bytes();
    T_REPORT();
}
