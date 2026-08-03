/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "providers/codex_settings.h"

static char *lookup(const char *contents, const char *key)
{
    return codex_toml_top_level_string(contents, strlen(contents), key);
}

static void expect_value(const char *contents, const char *key, const char *want)
{
    char *value = lookup(contents, key);
    EXPECT_STR_EQ(value, want);
    free(value);
}

static void expect_absent(const char *contents, const char *key)
{
    char *value = lookup(contents, key);
    EXPECT(value == NULL);
    free(value);
}

static void test_basic_assignment(void)
{
    expect_value("model = \"gpt-5.3-codex\"\n", "model", "gpt-5.3-codex");
    expect_value("model=\"o3\"\n", "model", "o3");
    expect_value("   model   =   \"o3\"   \n", "model", "o3");
    expect_value("model = 'o3'\n", "model", "o3");
}

static void test_key_selection(void)
{
    const char *contents = "model = \"a\"\nmodel_reasoning_effort = \"high\"\n";
    expect_value(contents, "model", "a");
    expect_value(contents, "model_reasoning_effort", "high");
    expect_absent(contents, "missing");
}

/* A prefix match must not satisfy a longer key, nor a longer line a shorter key. */
static void test_key_is_not_a_prefix_match(void)
{
    expect_absent("model_reasoning_effort = \"high\"\n", "model");
    expect_absent("mod = \"x\"\n", "model");
}

static void test_first_assignment_wins(void)
{
    expect_value("model = \"first\"\nmodel = \"second\"\n", "model", "first");
}

static void test_comments_ignored(void)
{
    expect_absent("# model = \"commented\"\n", "model");
    expect_value("# model = \"commented\"\nmodel = \"real\"\n", "model", "real");
}

/* Codex nests unrelated keys of the same name under tables; only the top level is ours. */
static void test_scanning_stops_at_first_table(void)
{
    expect_absent("[profiles.work]\nmodel = \"nested\"\n", "model");
    expect_value("model = \"top\"\n[profiles.work]\nmodel = \"nested\"\n", "model", "top");
    expect_absent("  [tui]\nmodel = \"nested\"\n", "model");
}

static void test_escape_sequences(void)
{
    expect_value("model = \"a\\tb\"\n", "model", "a\tb");
    expect_value("model = \"a\\nb\"\n", "model", "a\nb");
    expect_value("model = \"a\\\"b\"\n", "model", "a\"b");
    expect_value("model = \"a\\\\b\"\n", "model", "a\\b");
    expect_value("model = \"a\\bb\"\n", "model", "a\bb");
    expect_value("model = \"a\\fb\"\n", "model", "a\fb");
    expect_value("model = \"a\\rb\"\n", "model", "a\rb");
}

/* An unsupported escape keeps its byte rather than discarding an otherwise usable setting. */
static void test_unknown_escape_preserved(void)
{
    expect_value("model = \"a\\zb\"\n", "model", "azb");
}

/* Literal strings pass backslashes through verbatim. */
static void test_literal_string_has_no_escapes(void)
{
    expect_value("model = 'a\\tb'\n", "model", "a\\tb");
    expect_value("model = 'a\\'\n", "model", "a\\");
}

static void test_unterminated_string_rejected(void)
{
    expect_absent("model = \"unterminated\n", "model");
    expect_absent("model = 'unterminated\n", "model");
}

static void test_non_string_values_rejected(void)
{
    expect_absent("model = 3\n", "model");
    expect_absent("model = true\n", "model");
    expect_absent("model =\n", "model");
    expect_absent("model\n", "model");
}

static void test_empty_string_reported_verbatim(void)
{
    expect_value("model = \"\"\n", "model", "");
}

static void test_final_line_without_newline(void)
{
    expect_value("model = \"o3\"", "model", "o3");
}

static void test_empty_document(void)
{
    expect_absent("", "model");
    expect_absent("\n\n", "model");
}

/* The scanner is length-bounded, so a value may not read past the given extent. */
static void test_respects_contents_length(void)
{
    const char *contents = "model = \"o3\"\nmodel = \"ignored\"\n";
    char *value = codex_toml_top_level_string(contents, 12, "model");
    EXPECT_STR_EQ(value, "o3");
    free(value);

    EXPECT(codex_toml_top_level_string(contents, 0, "model") == NULL);
}

int main(void)
{
    test_basic_assignment();
    test_key_selection();
    test_key_is_not_a_prefix_match();
    test_first_assignment_wins();
    test_comments_ignored();
    test_scanning_stops_at_first_table();
    test_escape_sequences();
    test_unknown_escape_preserved();
    test_literal_string_has_no_escapes();
    test_unterminated_string_rejected();
    test_non_string_values_rejected();
    test_empty_string_reported_verbatim();
    test_final_line_without_newline();
    test_empty_document();
    test_respects_contents_length();
    T_REPORT();
}
