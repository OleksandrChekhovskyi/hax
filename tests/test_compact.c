/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "agent_core.h"
#include "compact.h"
#include "config.h"
#include "harness.h"
#include "util.h"

/* compact_over_threshold is the pure trigger predicate; everything else
 * (compact_should_auto, agent_compact) layers config + I/O on top of it. */
static void test_over_threshold(void)
{
    /* Unknown window (limit <= 0) or unreported ctx never triggers. */
    EXPECT(!compact_over_threshold(100, 0, 85));
    EXPECT(!compact_over_threshold(100, -1, 85));
    EXPECT(!compact_over_threshold(-1, 1000, 85));

    /* Below / at / above the percentage boundary. */
    EXPECT(!compact_over_threshold(8499, 10000, 85));
    EXPECT(compact_over_threshold(8500, 10000, 85));
    EXPECT(compact_over_threshold(9999, 10000, 85));

    /* 100% threshold only fires when fully at the window. */
    EXPECT(!compact_over_threshold(9999, 10000, 100));
    EXPECT(compact_over_threshold(10000, 10000, 100));
}

static void test_should_auto(void)
{
    /* Drive the config tiers via runtime overrides so the test is
     * hermetic regardless of env / file config. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "90");

    EXPECT(!compact_should_auto(8999, 10000)); /* 89.99% < 90% */
    EXPECT(compact_should_auto(9000, 10000));  /* exactly 90% */
    EXPECT(!compact_should_auto(9999, 0));     /* unknown window */

    /* Disabled via config: never auto-compacts, even when far over. */
    config_set_override("compact.auto", "0");
    EXPECT(!compact_should_auto(100000, 10000));

    /* Out-of-range threshold falls back to the 85% default. */
    config_set_override("compact.auto", "1");
    config_set_override("compact.threshold", "0");
    EXPECT(!compact_should_auto(8499, 10000));
    EXPECT(compact_should_auto(8500, 10000));
}

static void test_context_limit_resolution(void)
{
    /* Resolution order: manual config override → provider probe value →
     * model-catalog entry → 0 (unknown). The catalog tier reads a fixture
     * snapshot through the real catalog module. */
    unsetenv("HAX_CONTEXT_LIMIT");
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs("{\"openai\": {\"models\": {\"m\": {\"limit\": {\"context\": 64000}}}}}", f);
        fclose(f);
    }

    struct provider p = {.name = "x"};

    /* No probe value and no catalog identity: unknown. */
    EXPECT(compact_context_limit(&p, "m") == 0);

    /* The catalog fills in for a mapped provider — but only with a model
     * to key by, and only when the catalog knows it. */
    p.catalog_id = "openai";
    EXPECT(compact_context_limit(&p, "m") == 64000);
    EXPECT(compact_context_limit(&p, NULL) == 0);
    EXPECT(compact_context_limit(&p, "unknown-model") == 0);

    /* A probe value beats the catalog... */
    atomic_store(&p.context_limit, 32000);
    EXPECT(compact_context_limit(&p, "m") == 32000);

    /* ...and the manual override beats everything. */
    config_set_override("context_limit", "16k");
    EXPECT(compact_context_limit(&p, "m") == 16 * 1024);
    config_set_override("context_limit", NULL);
}

static void test_apply_seeds_history(void)
{
    /* compact_apply replaces history with exactly one user message that
     * wraps the summary in the seed preamble and carries the compact_seed
     * flag — the bit resume replay and the picker label key off. NULL logs:
     * persistence disabled, as in a HAX_NO_SESSION run. */
    struct agent_session s = {0};
    items_append(&s.items, &s.n_items, &s.cap_items,
                 (struct item){.kind = ITEM_USER_MESSAGE, .text = xstrdup("old prompt")});
    items_append(&s.items, &s.n_items, &s.cap_items,
                 (struct item){.kind = ITEM_ASSISTANT_MESSAGE, .text = xstrdup("old answer")});

    compact_apply(&s, NULL, NULL, "## Goal\n- finish the thing");

    EXPECT(s.n_items == 1);
    if (s.n_items == 1) {
        EXPECT(s.items[0].kind == ITEM_USER_MESSAGE);
        EXPECT(s.items[0].compact_seed);
        EXPECT(s.items[0].text && strstr(s.items[0].text, COMPACT_SEED_PREAMBLE) != NULL);
        EXPECT(s.items[0].text && strstr(s.items[0].text, "finish the thing") != NULL);
    }
    agent_session_free(&s);
}

static void test_usage_capture(void)
{
    struct compact_usage cu;
    compact_usage_init(&cu);
    struct stream_usage u = {.input_tokens = 100,
                             .output_tokens = 10,
                             .cached_tokens = -1,
                             .cache_write_tokens = -1,
                             .cache_write_1h_tokens = -1,
                             .cost = -1};
    compact_usage_record(&cu, &u);
    u.input_tokens = 200;
    compact_usage_record(&cu, &u);
    EXPECT(cu.n == 2);
    EXPECT(cu.att[0].u.input_tokens == 100);
    EXPECT(cu.att[1].u.input_tokens == 200);
    EXPECT(cu.att[0].ms >= 0 && cu.att[1].ms >= 0);

    /* Overflow past the cap drops silently rather than overrunning. */
    for (int i = 0; i < COMPACT_ATTEMPTS_MAX + 2; i++)
        compact_usage_record(&cu, &u);
    EXPECT(cu.n == COMPACT_ATTEMPTS_MAX);
}

int main(void)
{
    test_over_threshold();
    test_should_auto();
    test_context_limit_resolution();
    test_apply_seeds_history();
    test_usage_capture();
    T_REPORT();
}
