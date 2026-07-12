/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "catalog.h"
#include "config.h"
#include "harness.h"

/* catalog_lookup memoizes per (provider, model) for the process lifetime
 * (invalidated only by a background refresh), so each test case below uses
 * distinct model ids — a reloaded config tier must not be expected to
 * change an already-memoized answer. */

/* Point the cache tier at a private temp tree and write `json` as the
 * cached catalog snapshot. */
static void write_cache_fixture(const char *json)
{
    static char dir[] = "/tmp/hax_test_catalog_XXXXXX";
    static int made;
    if (!made) {
        if (!mkdtemp(dir))
            FAIL("mkdtemp: %s", strerror(errno));
        setenv("XDG_CACHE_HOME", dir, 1);
        made = 1;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        FAIL("fopen %s: %s", path, strerror(errno));
    fputs(json, f);
    fclose(f);
}

static const char CACHE_FIXTURE[] =
    "{"
    "  \"openai\": {\"id\": \"openai\", \"models\": {"
    "    \"o3\": {\"cost\": {\"input\": 2, \"output\": 8, \"cache_read\": 0.5},"
    "             \"limit\": {\"context\": 200000, \"output\": 100000}},"
    "    \"o3-merge\": {\"cost\": {\"input\": 2, \"output\": 8},"
    "                   \"limit\": {\"context\": 200000}}"
    "  }},"
    "  \"openrouter\": {\"models\": {"
    "    \"vendor/model.v1:free\": {\"cost\": {\"input\": 0.1, \"output\": 0.2},"
    "                               \"limit\": {\"context\": 131072}}"
    "  }}"
    "}";

static void test_lookup_from_cache(void)
{
    struct catalog_entry e;
    EXPECT(catalog_lookup("openai", "o3", &e) == 0);
    EXPECT(e.cost_input == 2);
    EXPECT(e.cost_output == 8);
    EXPECT(e.cost_cache_read == 0.5);
    EXPECT(e.cost_cache_write == -1); /* not declared */
    EXPECT(e.context == 200000);
    EXPECT(e.output == 100000);
}

static void test_lookup_dotted_slashed_model_id(void)
{
    /* Model ids with '/', '.', and ':' are plain object keys in both
     * tiers — no dotted-key splitting may apply to them. */
    struct catalog_entry e;
    EXPECT(catalog_lookup("openrouter", "vendor/model.v1:free", &e) == 0);
    EXPECT(e.cost_input == 0.1);
    EXPECT(e.context == 131072);
}

static void test_lookup_miss(void)
{
    struct catalog_entry e;
    EXPECT(catalog_lookup("openai", "no-such-model", &e) == -1);
    EXPECT(e.cost_input == -1);
    EXPECT(e.context == 0);
    EXPECT(catalog_lookup("no-such-provider", "o3", &e) == -1);
    EXPECT(catalog_lookup(NULL, "o3", &e) == -1);
    EXPECT(catalog_lookup("openai", NULL, &e) == -1);
}

static void test_config_overrides_and_merges(void)
{
    /* The config catalog.models block wins field-by-field; the cache fills
     * what it leaves unset. Numbers arrive normalized to strings (config.c),
     * and token counts accept the parse_size grammar. */
    EXPECT(config_load("{\"catalog\": {\"models\": {\"openai\": {"
                       "  \"gpt-x.5\": {\"cost\": {\"input\": 1.25, \"output\": 10},"
                       "                \"limit\": {\"context\": \"256k\"}},"
                       "  \"o3-merge\": {\"cost\": {\"input\": 99}}"
                       "}}}}") == 0);

    /* Config-only entry (model unknown to the cache). */
    struct catalog_entry e;
    EXPECT(catalog_lookup("openai", "gpt-x.5", &e) == 0);
    EXPECT(e.cost_input == 1.25);
    EXPECT(e.cost_output == 10);
    EXPECT(e.context == 256 * 1024);
    EXPECT(e.output == 0);

    /* Config + cache merge: config's input rate wins, the rest fills in
     * from the cached snapshot. */
    EXPECT(catalog_lookup("openai", "o3-merge", &e) == 0);
    EXPECT(e.cost_input == 99);
    EXPECT(e.cost_output == 8);
    EXPECT(e.context == 200000);

    config_load(NULL);
}

static void test_price_formula(void)
{
    struct catalog_entry e = {
        .cost_input = 2,
        .cost_output = 8,
        .cost_cache_read = 0.5,
        .cost_cache_write = 2.5,
        .context = 0,
        .output = 0,
    };
    /* 1M uncached input + 1M output at base rates. */
    EXPECT(catalog_price(&e, 1000000, 1000000, 0, 0, 0) == 10.0);
    /* Cached reads and writes are subsets of input, priced at their own
     * rates: 1M input of which 500k reads + 250k writes leaves 250k at
     * the input rate. 0.5M*0.5 + 0.25M*2.5 + 0.25M*2 = 1.375. */
    EXPECT(catalog_price(&e, 1000000, 0, 500000, 250000, 0) == 1.375);
    /* The 1h subset of the writes bills at 2x input instead of the
     * (5-minute) cache_write rate: of the 250k writes, 100k are 1h.
     * 0.25M*2 + 0.5M*0.5 + 0.15M*2.5 + 0.1M*(2*2) = 1.525. */
    EXPECT(catalog_price(&e, 1000000, 0, 500000, 250000, 100000) == 1.525);
    /* A 1h count exceeding the writes clamps to them (subset contract):
     * 0.9M*2 + 0.1M*(2*2) = 2.2. */
    EXPECT(catalog_price(&e, 1000000, 0, 0, 100000, 200000) == 2.2);
    /* Negative ("not reported") counts read as zero. */
    EXPECT(catalog_price(&e, 1000000, -1, -1, -1, -1) == 2.0);
    /* Unknown cache rates fall back to the input rate. */
    e.cost_cache_read = -1;
    e.cost_cache_write = -1;
    EXPECT(catalog_price(&e, 1000000, 0, 400000, 0, 0) == 2.0);
    /* Unknown input or output rate: no estimate at all. */
    e.cost_input = -1;
    EXPECT(catalog_price(&e, 1000000, 1000000, 0, 0, 0) == -1);
}

static void test_extract_member(void)
{
    /* Keys match exactly (no prefix hits), later members are reachable
     * past earlier ones, and escaped quotes/braces inside strings don't
     * derail the byte scan. */
    const char *text = "{\n"
                       "  \"open\": {\"models\": {}},\n"
                       "  \"tricky\": {\"s\": \"esc \\\" } ] {\", \"a\": [1, {\"b\": []}]},\n"
                       "  \"openai\": {\"models\": {\"m\": {\"cost\": {\"input\": 2}}}, \"n\": 1}\n"
                       "}";
    json_t *v = catalog_extract_member(text, "openai");
    EXPECT(v != NULL);
    if (v) {
        EXPECT(json_is_object(json_object_get(v, "models")));
        json_decref(v);
    }
    v = catalog_extract_member(text, "tricky");
    EXPECT(v != NULL);
    if (v) {
        EXPECT_STR_EQ(json_string_value(json_object_get(v, "s")), "esc \" } ] {");
        json_decref(v);
    }
    /* Scalar member values come back too (JSON_DECODE_ANY). */
    v = catalog_extract_member("{\"n\": 42}", "n");
    EXPECT(v != NULL && json_is_integer(v) && json_integer_value(v) == 42);
    json_decref(v);

    /* Misses: absent key, prefix-of-a-key, wrong roots, truncation. */
    EXPECT(catalog_extract_member(text, "ope") == NULL);
    EXPECT(catalog_extract_member(text, "openai2") == NULL);
    EXPECT(catalog_extract_member(text, "models") == NULL); /* nested, not top-level */
    EXPECT(catalog_extract_member("[1, 2]", "k") == NULL);
    EXPECT(catalog_extract_member("null", "k") == NULL);
    EXPECT(catalog_extract_member("{}", "k") == NULL);
    EXPECT(catalog_extract_member("{", "k") == NULL);
    EXPECT(catalog_extract_member("{\"k\": {\"a\": 1}", "k") == NULL); /* unterminated root */
    EXPECT(catalog_extract_member("{\"k\": \"unterminated", "k") == NULL);
    EXPECT(catalog_extract_member(NULL, "k") == NULL);
    EXPECT(catalog_extract_member("{}", NULL) == NULL);
}

static void test_prefetch_disabled_is_noop(void)
{
    /* Empty catalog.url opts out of fetching (and of the staleness alarm —
     * the user chose no refreshes); prefetch spawns nothing and shutdown
     * stays safe. The fetch worker itself — download, validation,
     * generation bump, staleness reporting — is covered end to end in
     * tests/test_catalog_fetch.c. */
    setenv("HAX_CATALOG_URL", "", 1);
    EXPECT(catalog_prefetch() == 0);
    EXPECT(catalog_prefetch() == 0); /* once-latched, still safe */
    catalog_shutdown();
    unsetenv("HAX_CATALOG_URL");
}

static void test_memoization_and_shutdown_clear(void)
{
    /* A pair's cache-tier answer is memoized: rewriting the snapshot on
     * disk must NOT change it until something invalidates the memo — a
     * background refresh (generation bump, covered in test_catalog_fetch)
     * or catalog_shutdown. Distinguishes real memoization from a silent
     * re-parse. Runs last: it replaces the shared fixture. */
    struct catalog_entry e;
    EXPECT(catalog_lookup("openai", "o3", &e) == 0);
    EXPECT(e.cost_input == 2);
    write_cache_fixture("{\"openai\": {\"models\": {"
                        "\"o3\": {\"cost\": {\"input\": 5, \"output\": 8}}}}}");
    EXPECT(catalog_lookup("openai", "o3", &e) == 0);
    EXPECT(e.cost_input == 2); /* memo hit, not the rewritten file */
    catalog_shutdown();        /* joins workers, clears the memo */
    EXPECT(catalog_lookup("openai", "o3", &e) == 0);
    EXPECT(e.cost_input == 5); /* fresh parse sees the new snapshot */
}

int main(void)
{
    write_cache_fixture(CACHE_FIXTURE);

    test_lookup_from_cache();
    test_lookup_dotted_slashed_model_id();
    test_lookup_miss();
    test_config_overrides_and_merges();
    test_price_formula();
    test_extract_member();
    test_prefetch_disabled_is_noop();
    test_memoization_and_shutdown_clear();

    catalog_shutdown();
    T_REPORT();
}
