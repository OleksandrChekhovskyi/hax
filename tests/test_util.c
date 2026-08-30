/* SPDX-License-Identifier: MIT */
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util.h"

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

/* ---------- allocation ---------- */

static void test_string_array_concat(void)
{
    EXPECT(string_array_concat(NULL, NULL) == NULL);

    /* A leading NULL entry terminates the array: {NULL} counts as empty. */
    const char *empty[] = {NULL};
    EXPECT(string_array_concat(empty, NULL) == NULL);

    const char *first[] = {"a: 1", NULL};
    const char *second[] = {"b: 2", "c: 3", NULL};
    char **combined = string_array_concat(first, second);
    EXPECT(combined != NULL);
    if (combined) {
        EXPECT_STR_EQ(combined[0], "a: 1");
        EXPECT_STR_EQ(combined[1], "b: 2");
        EXPECT_STR_EQ(combined[2], "c: 3");
        EXPECT(combined[3] == NULL);
        string_array_free(combined);
    }

    char **only_second = string_array_concat(NULL, second);
    EXPECT(only_second != NULL);
    if (only_second) {
        EXPECT_STR_EQ(only_second[0], "b: 2");
        EXPECT_STR_EQ(only_second[1], "c: 3");
        EXPECT(only_second[2] == NULL);
        string_array_free(only_second);
    }
}

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

/* ---------- format_duration / format_cost ---------- */

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

static void test_format_duration_extreme(void)
{
    char formatted[64];
    format_duration(formatted, sizeof(formatted), LONG_MAX);
    EXPECT(formatted[0] != '-');
    EXPECT(strchr(formatted, 'h') != NULL);
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

    test_uuid_v4_format();
    test_uuid_v4_unique();

    test_string_array_concat();
    test_zero_sized_allocations();

    test_parse_int();

    test_format_duration_ranges();
    test_format_duration_extreme();
    test_format_cost_precision();

    test_shell_single_quote();

    T_REPORT();
}
