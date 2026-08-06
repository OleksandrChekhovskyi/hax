/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "terminal/clipboard.h"

static void expect_osc52(const char *text, size_t text_len, int tmux_wrap, const char *expected)
{
    size_t sequence_len;
    char *sequence = clipboard_osc52_sequence(text, text_len, tmux_wrap, &sequence_len);
    EXPECT(sequence != NULL);
    if (!sequence)
        return;

    size_t expected_len = strlen(expected);
    EXPECT(sequence_len == expected_len);
    if (sequence_len == expected_len)
        EXPECT(memcmp(sequence, expected, expected_len) == 0);
    free(sequence);
}

static void test_osc52_basic(void)
{
    expect_osc52("hello", 5, 0, "\x1b]52;c;aGVsbG8=\x07");
}

static void test_osc52_empty_payload(void)
{
    expect_osc52("", 0, 0, "\x1b]52;c;\x07");
}

static void test_osc52_tmux_passthrough(void)
{
    expect_osc52("hello", 5, 1, "\x1bPtmux;\x1b\x1b]52;c;aGVsbG8=\x07\x1b\\");
}

static void test_osc52_rejects_oversized_payload(void)
{
    size_t oversized_len = CLIPBOARD_OSC52_MAX_BYTES + 1;
    char *text = malloc(oversized_len);
    EXPECT(text != NULL);
    if (!text)
        return;
    memset(text, 'x', oversized_len);

    char *sequence = clipboard_osc52_sequence(text, oversized_len, 0, NULL);
    EXPECT(sequence == NULL);

    sequence = clipboard_osc52_sequence(text, CLIPBOARD_OSC52_MAX_BYTES, 0, NULL);
    EXPECT(sequence != NULL);
    free(sequence);
    free(text);
}

static void test_osc52_base64_padding(void)
{
    expect_osc52("f", 1, 0, "\x1b]52;c;Zg==\x07");
    expect_osc52("fo", 2, 0, "\x1b]52;c;Zm8=\x07");
    expect_osc52("foo", 3, 0, "\x1b]52;c;Zm9v\x07");
}

static void test_osc52_embedded_nul(void)
{
    const char text[] = {'a', '\0', 'b'};
    expect_osc52(text, sizeof(text), 0, "\x1b]52;c;YQBi\x07");
}

static void test_osc52_high_bytes(void)
{
    const unsigned char text[] = {0xc3, 0xa9, 0xe2, 0x9c, 0x93}; /* "é✓" */
    expect_osc52((const char *)text, sizeof(text), 0, "\x1b]52;c;w6ninJM=\x07");
}

int main(void)
{
    test_osc52_basic();
    test_osc52_empty_payload();
    test_osc52_tmux_passthrough();
    test_osc52_rejects_oversized_payload();
    test_osc52_base64_padding();
    test_osc52_embedded_nul();
    test_osc52_high_bytes();
    T_REPORT();
}
