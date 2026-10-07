/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "system/locale.h"
#include "text/display_safe.h"

static void test_sanitize_replaces_escape_sequences(void)
{
    const char *unsafe = "safe\x1b[2J\x1b[Hgone";
    char *output = sanitize_for_display(unsafe, strlen(unsafe));
    EXPECT(strchr(output, 0x1b) == NULL);
    EXPECT_STR_EQ(output, "safe?[2J?[Hgone");
    free(output);
}

static void test_sanitize_replaces_controls_and_keeps_utf8(void)
{
    const char *controls = "a\rb\ac";
    char *output = sanitize_for_display(controls, strlen(controls));
    EXPECT_STR_EQ(output, "a?b?c");
    free(output);

    if (!locale_have_utf8())
        return;

    output = sanitize_for_display("c – ü", strlen("c – ü"));
    EXPECT_STR_EQ(output, "c – ü");
    free(output);
}

static void test_sanitize_accepts_counted_text(void)
{
    char *output = sanitize_for_display("abcdef", 3);
    EXPECT_STR_EQ(output, "abc");
    free(output);
}

int main(void)
{
    locale_init_utf8();

    test_sanitize_replaces_escape_sequences();
    test_sanitize_replaces_controls_and_keeps_utf8();
    test_sanitize_accepts_counted_text();
    T_REPORT();
}
