/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "system/locale.h"
#include "text/display_safe.h"

/* ---------- sanitize_for_display ---------- */

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

static void test_flatten_preserves_high_bytes(void)
{
    /* Printable UTF-8 passes through. */
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

int main(void)
{
    locale_init_utf8();

    test_sanitize_replaces_escape_sequences();
    test_sanitize_replaces_controls_and_keeps_utf8();
    test_sanitize_accepts_counted_text();

    test_flatten_null();
    test_flatten_empty();
    test_flatten_plain();
    test_flatten_newline();
    test_flatten_collapses_runs();
    test_flatten_strips_edges();
    test_flatten_all_whitespace();
    test_flatten_control_bytes();
    test_flatten_preserves_high_bytes();
    test_flatten_substitutes_bidi_override();
    test_flatten_substitutes_zwj();
    test_flatten_substitutes_malformed_utf8();
    test_flatten_caps_zero_width_run();
    test_flatten_preserves_legit_combining_run();

    T_REPORT();
}
