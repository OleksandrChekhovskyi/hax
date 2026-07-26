/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "catalog.h"
#include "config.h"
#include "harness.h"
#include "model_meta.h"
#include "provider.h"
#include "providers/registry.h"

/* The resolution policy — config over the live report over the models.dev
 * snapshot over the provider's own defaults — and the clamp. The effort
 * cases run without a catalog (their providers declare no catalog_id), so
 * what they pin is the live-vs-static layering and the ordering rule; the
 * context and image cases add a fixture snapshot to exercise the tier below
 * that. The catalog's own parsing is covered in test_catalog.c. */

/* Point the cache tier at a private tree holding one model. */
static void write_catalog_fixture(void)
{
    char *dir = t_tempdir();
    setenv("XDG_CACHE_HOME", dir, 1);
    char path[600];
    snprintf(path, sizeof(path), "%s/hax", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/hax/catalog.json", dir);
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs("{\"openai\": {\"models\": {\"m\": {\"limit\": {\"context\": 64000},"
              "\"modalities\": {\"input\": [\"text\", \"image\"]}},"
              "\"priced\": {\"cost\": {\"input\": 2, \"output\": 8, \"cache_read\": 0.5,"
              " \"tiers\": [{\"tier\": {\"type\": \"context\", \"size\": 200000},"
              " \"input\": 4, \"output\": 16}]}},"
              "\"foreign-ladder\": {\"reasoning_options\":"
              " [{\"type\": \"effort\", \"values\": [\"minimal\", \"low\", \"high\"]}]}}}}",
              f);
        fclose(f);
    }
}

static const char *const LADDER[] = {"none", "low", "medium", "high", "xhigh"};

static size_t fake_list_efforts(struct provider *p, const char *const **out)
{
    (void)p;
    *out = LADDER;
    return sizeof(LADDER) / sizeof(LADDER[0]);
}

static size_t no_efforts(struct provider *p, const char *const **out)
{
    (void)p;
    (void)out;
    return 0;
}

static struct provider make_provider(const char *name,
                                     size_t (*list)(struct provider *, const char *const **))
{
    struct provider p;
    memset(&p, 0, sizeof(p));
    p.name = name;
    p.list_efforts = list;
    return p;
}

/* Remember `levels` (NULL-terminated) as what the backend reported for
 * `model`; a NULL list reports "no levels at all". */
static void remember_efforts(struct provider *p, const char *model, const char *const *levels)
{
    struct model_info m;
    model_info_init(&m);
    m.id = xstrdup(model);
    m.efforts.known = 1;
    for (size_t i = 0; levels && levels[i]; i++)
        effort_set_add(&m.efforts, levels[i]);
    model_meta_remember(p, &m);
    model_info_clear(&m);
}

/* Remember `m` as the live report for its id, taking a copy so the caller
 * keeps ownership of what it built. */
static void remember_rates(struct provider *p, const struct model_info *m)
{
    struct model_info copy;
    model_info_copy(&copy, m);
    model_meta_remember(p, &copy);
    model_info_clear(&copy);
}

/* The rate view pricing bills against: live over catalog, with tiers
 * moving whole rather than field by field. */
static void test_rates_resolution(void)
{
    struct provider p = make_provider("x", NULL);
    struct catalog_entry e;

    /* Nothing knows anything: not priceable, and the fields say so rather
     * than reading as free. */
    EXPECT(model_meta_rates(&p, "priced", &e) == 0);
    EXPECT(e.cost_input < 0 && e.cost_output < 0);
    EXPECT(e.cost_cache_write_1h < 0);
    EXPECT(e.n_tiers == 0);

    /* Catalog tier alone, tiers included. */
    p.catalog_id = "openai";
    EXPECT(model_meta_rates(&p, "priced", &e) == 1);
    EXPECT(e.cost_input == 2 && e.cost_output == 8 && e.cost_cache_read == 0.5);
    EXPECT(e.n_tiers == 1 && e.tiers[0].above == 200000);
    EXPECT(catalog_price(&e, 1000000, 0, 0, 0, 0, NULL) == 4.0); /* over the tier */

    /* The backend's own rates win: it quotes what it will actually charge
     * (a router's margin included), where the snapshot quotes the
     * upstream's list price. */
    struct model_info live;
    model_info_init(&live);
    live.id = xstrdup("priced");
    live.cost_input = 3;
    live.cost_output = 12;
    live.cost_cache_read = 0.3;
    live.cost_cache_write = 3.75;
    live.cost_cache_write_1h = 6;
    remember_rates(&p, &live);
    EXPECT(model_meta_rates(&p, "priced", &e) == 1);
    EXPECT(e.cost_input == 3 && e.cost_output == 12);
    EXPECT(e.cost_cache_read == 0.3 && e.cost_cache_write == 3.75);
    EXPECT(e.cost_cache_write_1h == 6);
    /* The catalog's thresholds must NOT attach to the backend's base
     * rates — that would bill a request at rates that never coexisted. A
     * backend quoting rates and no tiers is priced flat. */
    EXPECT(e.n_tiers == 0);
    EXPECT(catalog_price(&e, 1000000, 0, 0, 0, 0, NULL) == 3.0);

    /* A backend that quotes tiers of its own keeps them. */
    live.n_tiers = 1;
    live.tiers[0] = (struct catalog_tier){.above = 99999,
                                          .cost_input = 6,
                                          .cost_output = -1,
                                          .cost_cache_read = -1,
                                          .cost_cache_write = -1,
                                          .cost_cache_write_1h = -1};
    remember_rates(&p, &live);
    EXPECT(model_meta_rates(&p, "priced", &e) == 1);
    EXPECT(e.n_tiers == 1 && e.tiers[0].above == 99999);
    /* The tier's undeclared output rate falls back to the base rate. */
    EXPECT(catalog_price(&e, 1000000, 1000000, 0, 0, 0, NULL) == 18.0);

    model_info_clear(&live);
    model_meta_release(&p);
}

static void test_effort_set_basics(void)
{
    struct effort_set s = {0};
    EXPECT(!s.known);
    EXPECT(!effort_set_has(&s, "low"));

    EXPECT(effort_set_add(&s, "low") == 1);
    EXPECT(s.known && s.n == 1 && effort_set_has(&s, "low"));
    /* Duplicates collapse: two sources naming the same level must not
     * produce two rows. */
    EXPECT(effort_set_add(&s, "low") == 0);
    EXPECT(s.n == 1);
    /* Rejected candidates still answer the question — a set can be known
     * and empty, which is what "this model takes no effort levels" is. */
    struct effort_set empty = {0};
    EXPECT(effort_set_add(&empty, "") == 0);
    EXPECT(empty.known && empty.n == 0);
    /* A value too long to store is dropped rather than truncated: a
     * truncated level would be sent verbatim and rejected. */
    EXPECT(effort_set_add(&s, "an-absurdly-long-effort-name") == 0);
    EXPECT(s.n == 1);
    for (int i = 0; i < EFFORT_MAX_LEVELS + 3; i++) {
        char buf[8];
        snprintf(buf, sizeof buf, "l%d", i);
        effort_set_add(&s, buf);
    }
    EXPECT(s.n == EFFORT_MAX_LEVELS);
}

/* Resolution order for the context window: the manual override, then the
 * live answer, then the catalog. */
static void test_context_resolution(void)
{
    unsetenv("HAX_CONTEXT_LIMIT");
    struct provider p = make_provider("x", NULL);

    /* Nothing live and no catalog identity: unknown. */
    EXPECT(model_meta_context(&p, "m") == 0);

    /* The catalog answers for a mapped provider — but only with a model to
     * key by, and only one it knows. */
    p.catalog_id = "openai";
    EXPECT(model_meta_context(&p, "m") == 64000);
    EXPECT(model_meta_context(&p, NULL) == 0);
    EXPECT(model_meta_context(&p, "unknown-model") == 0);

    /* A live answer beats the catalog, for the model it describes. */
    struct model_info live;
    model_info_init(&live);
    live.id = xstrdup("m");
    live.context = 32000;
    model_meta_remember(&p, &live);
    model_info_clear(&live);
    EXPECT(model_meta_context(&p, "m") == 32000);
    EXPECT(model_meta_context(&p, "other") == 0);

    /* The manual override beats everything. */
    config_set_override("context_limit", "16k");
    EXPECT(model_meta_context(&p, "m") == 16 * 1024);
    config_set_override("context_limit", NULL);
    model_meta_release(&p);
}

/* Same ladder for image input, which additionally answers as a tristate. */
static void test_image_input_resolution(void)
{
    unsetenv("HAX_IMAGE_INPUT");
    struct provider p = make_provider("x", NULL);
    EXPECT(model_meta_image_input(&p, "m") == -1);
    EXPECT(model_meta_image_input(NULL, NULL) == -1);

    p.catalog_id = "openai";
    EXPECT(model_meta_image_input(&p, "m") == 1); /* fixture declares an image modality */

    /* A live "no" overrides the catalog's "yes" — llama.cpp vision depends
     * on the mmproj loaded into this server, which no snapshot can know. */
    struct model_info live;
    model_info_init(&live);
    live.id = xstrdup("m");
    live.image_input = PROVIDER_CAP_NO;
    model_meta_remember(&p, &live);
    model_info_clear(&live);
    EXPECT(model_meta_image_input(&p, "m") == 0);
    /* Scoped to the model it describes; nothing knows "other". */
    EXPECT(model_meta_image_input(&p, "other") == -1);

    /* The config tristate pins it; "auto" falls back through to detection. */
    setenv("HAX_IMAGE_INPUT", "on", 1);
    EXPECT(model_meta_image_input(&p, "m") == 1);
    setenv("HAX_IMAGE_INPUT", "auto", 1);
    EXPECT(model_meta_image_input(&p, "m") == 0);
    unsetenv("HAX_IMAGE_INPUT");
    model_meta_release(&p);
}

static int is(const struct effort_set *s, const char *const *want)
{
    size_t n = 0;
    while (want[n])
        n++;
    if (!s->known || s->n != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(s->v[i], want[i]) != 0)
            return 0;
    return 1;
}

static void test_falls_back_to_static_ladder(void)
{
    /* Nothing known about the model: the provider's full ladder stands. */
    struct provider p = make_provider("codex", fake_list_efforts);
    struct effort_set s;
    model_meta_efforts(&p, "gpt-unknown", &s);
    static const char *const want[] = {"none", "low", "medium", "high", "xhigh", NULL};
    EXPECT(is(&s, want));
    model_meta_release(&p);
}

static void test_live_report_narrows(void)
{
    static const char *const reported[] = {"low", "medium", "high", NULL};
    struct provider p = make_provider("codex", fake_list_efforts);
    remember_efforts(&p, "gpt-narrow", reported);
    struct effort_set s;
    model_meta_efforts(&p, "gpt-narrow", &s);
    static const char *const want[] = {"low", "medium", "high", NULL};
    EXPECT(is(&s, want));
    /* The two the model doesn't take are gone — on codex and Anthropic
     * sending either is a 400 on every turn, not a downgrade. */
    EXPECT(!effort_set_has(&s, "none"));
    EXPECT(!effort_set_has(&s, "xhigh"));
    model_meta_release(&p);
}

static void test_live_report_can_add_beyond_the_ladder(void)
{
    /* "max" post-dates the openai-family ladder, and OpenRouter reports it
     * for models that take it. A reported level the ladder can't order is
     * appended rather than dropped — which is what makes a model released
     * after this build usable on the day it appears. */
    static const char *const reported[] = {"max", "high", "medium", "low", NULL};
    struct provider p = make_provider("openrouter", fake_list_efforts);
    remember_efforts(&p, "vendor/new", reported);
    struct effort_set s;
    model_meta_efforts(&p, "vendor/new", &s);
    static const char *const want[] = {"low", "medium", "high", "max", NULL};
    EXPECT(is(&s, want));
    model_meta_release(&p);
}

/* The catalog gets the opposite treatment, and for a reason: its id is
 * shared, so a codex model resolves against models.dev's "openai" entry —
 * whose "minimal" the codex backend answers 400 for. It may narrow the
 * ladder; it may not add to it. */
static void test_catalog_narrows_but_cannot_widen(void)
{
    struct provider p = make_provider("codex", fake_list_efforts);
    p.catalog_id = "openai";
    struct effort_set s;
    model_meta_efforts(&p, "foreign-ladder", &s);
    static const char *const want[] = {"low", "high", NULL};
    EXPECT(is(&s, want));
    EXPECT(!effort_set_has(&s, "minimal"));
    model_meta_release(&p);
}

static void test_reported_empty_means_no_ladder(void)
{
    /* A budget-mode or non-reasoning model: the backend answered, and the
     * answer is "no levels". The effort step disappears instead of
     * offering the static ladder. */
    struct provider p = make_provider("anthropic", fake_list_efforts);
    remember_efforts(&p, "claude-budget", NULL);
    struct effort_set s;
    model_meta_efforts(&p, "claude-budget", &s);
    EXPECT(s.known && s.n == 0);
    model_meta_release(&p);
}

static void test_entry_is_scoped_to_provider_and_model(void)
{
    /* Metadata belongs to the provider that reported it. Two providers are
     * live at once whenever /provider builds a prospective backend and runs
     * its pickers while the current one still serves, so one must never
     * answer with the other's numbers — nor with a previous model's. */
    static const char *const reported[] = {"low", NULL};
    struct provider same = make_provider("codex", fake_list_efforts);
    struct provider other = make_provider("openrouter", fake_list_efforts);
    remember_efforts(&same, "gpt-narrow", reported);
    struct effort_set s;

    model_meta_efforts(&other, "gpt-narrow", &s); /* right model, other provider */
    EXPECT(s.n == 5);
    model_meta_efforts(&same, "gpt-other", &s); /* right provider, wrong model */
    EXPECT(s.n == 5);
    model_meta_efforts(&same, "gpt-narrow", &s);
    EXPECT(s.n == 1);

    /* Releasing one leaves the other's answer intact. */
    model_meta_release(&other);
    model_meta_efforts(&same, "gpt-narrow", &s);
    EXPECT(s.n == 1);
    model_meta_release(&same);
}

/* The /model picker publishes its candidate before the user has committed
 * to it, so an Escape at the chained effort step has to put back what the
 * running model had — without a second round-trip, which would leave a
 * window, and a permanent gap when it fails. */
static void test_snapshot_restores_the_displaced_entry(void)
{
    static const char *const running[] = {"low", NULL};
    static const char *const candidate[] = {"low", "medium", "high", NULL};
    struct provider p = make_provider("openrouter", fake_list_efforts);
    remember_efforts(&p, "model-running", running);

    struct model_info saved;
    EXPECT(model_meta_snapshot(&p, &saved) == 1);
    EXPECT_STR_EQ(saved.id, "model-running");

    /* The picker's hand-over, then the abort. */
    remember_efforts(&p, "model-candidate", candidate);
    struct effort_set s;
    model_meta_efforts(&p, "model-running", &s);
    EXPECT(s.n == 5); /* displaced: back to the static ladder */

    model_meta_remember(&p, &saved);
    model_info_clear(&saved);
    model_meta_efforts(&p, "model-running", &s);
    EXPECT(s.n == 1); /* and back again, with no refetch */
    model_meta_efforts(&p, "model-candidate", &s);
    EXPECT(s.n == 5); /* the candidate is gone, as an abort demands */

    /* Nothing held answers 0 and leaves *out safe to clear. */
    model_meta_release(&p);
    struct model_info empty;
    EXPECT(model_meta_snapshot(&p, &empty) == 0);
    model_info_clear(&empty);
}

/* The /model picker hands its row over on the way past, which is what
 * saves a redundant fetch on a backend whose list is rich. A list of bare
 * ids has nothing to hand over, and adopting one anyway would look like an
 * answer for that model — suppressing the probe that has the real one.
 * llama.cpp is the case: /v1/models is ids, /props is everything. */
static void test_bare_row_leaves_the_probe_to_run(void)
{
    unsetenv("HAX_CONTEXT_LIMIT");
    struct provider p = make_provider("llama.cpp", no_efforts);
    struct model_info bare;
    model_info_init(&bare);
    bare.id = xstrdup("qwen3.gguf");
    model_meta_remember(&p, &bare);
    model_info_clear(&bare);

    struct model_info held;
    EXPECT(model_meta_snapshot(&p, &held) == 0); /* nothing adopted */
    model_info_clear(&held);

    /* One real fact makes the row worth keeping. */
    struct model_info props;
    model_info_init(&props);
    props.id = xstrdup("qwen3.gguf");
    props.context = 256000;
    model_meta_remember(&p, &props);
    model_info_clear(&props);
    EXPECT(model_meta_context(&p, "qwen3.gguf") == 256000);
    model_meta_release(&p);
}

static void test_provider_without_a_ladder_stays_without_one(void)
{
    /* llama.cpp and friends send no effort field at all. Metadata about
     * the model can't change that, so the question is settled before the
     * live tier is consulted. */
    static const char *const reported[] = {"low", "high", NULL};
    struct provider p = make_provider("llama.cpp", no_efforts);
    remember_efforts(&p, "qwen3", reported);
    struct effort_set s;
    model_meta_efforts(&p, "qwen3", &s);
    EXPECT(s.known && s.n == 0);
    model_meta_release(&p);
}

/* Every provider's destroy() owes a model_meta_release (provider.h), and
 * the refresh path runs against whatever backend is live — including ones
 * with no probe_model hook at all. Construct every provider that will build
 * in a test environment, refresh it, and destroy it: a slot left behind
 * shows up as a leak under ASan. */
static void test_release_is_honored_by_every_provider(void)
{
    /* provider_all filters the internal mock out, so walk it explicitly
     * too — a dev backend still has to honor the contract. */
    size_t n = 0;
    const struct provider_factory *const *facs = provider_all(&n);
    EXPECT(n > 0);
    int built = 0;
    for (size_t i = 0; i <= n; i++) {
        const struct provider_factory *f = (i < n) ? facs[i] : provider_find("mock");
        EXPECT(f != NULL);
        struct provider *p = f ? f->new(f->name) : NULL;
        if (!p)
            continue; /* needs credentials or a live server — not here */
        built++;
        /* Both ways a slot comes into existence: the refresh path (which
         * allocates only for a backend that can probe) and the picker
         * handing an entry over (which always does). */
        model_meta_refresh(p, "some-model");
        struct model_info m;
        model_info_init(&m);
        m.id = xstrdup("some-model");
        model_meta_remember(p, &m);
        model_info_clear(&m);
        p->destroy(p);
    }
    /* The mock always constructs, so this can't silently pass by building
     * nothing when no credentials are present. */
    EXPECT(built > 0);
}

static void test_clamp(void)
{
    struct effort_set s = {0};
    effort_set_add(&s, "low");
    effort_set_add(&s, "high");

    /* Present: kept verbatim. */
    EXPECT_STR_EQ(effort_clamp(&s, "high"), "high");
    /* Above everything offered: down to the nearest, so a persisted
     * "xhigh" lands on "high" rather than reverting to the default. */
    EXPECT_STR_EQ(effort_clamp(&s, "xhigh"), "high");
    /* Between two offered levels: down, the cheaper neighbor. */
    EXPECT_STR_EQ(effort_clamp(&s, "medium"), "low");
    /* Below everything offered: up, since there is nothing under it. */
    EXPECT_STR_EQ(effort_clamp(&s, "none"), "low");
    /* A name with no place in the ladder can't be positioned, so no
     * guess is made. */
    EXPECT(effort_clamp(&s, "ludicrous") == NULL);

    struct effort_set empty = {.known = 1};
    EXPECT(effort_clamp(&empty, "low") == NULL);
    struct effort_set unknown = {0};
    EXPECT(effort_clamp(&unknown, "low") == NULL);
}

int main(void)
{
    write_catalog_fixture();
    test_effort_set_basics();
    test_rates_resolution();
    test_context_resolution();
    test_image_input_resolution();
    test_falls_back_to_static_ladder();
    test_live_report_narrows();
    test_live_report_can_add_beyond_the_ladder();
    test_catalog_narrows_but_cannot_widen();
    test_reported_empty_means_no_ladder();
    test_entry_is_scoped_to_provider_and_model();
    test_snapshot_restores_the_displaced_entry();
    test_bare_row_leaves_the_probe_to_run();
    test_provider_without_a_ladder_stays_without_one();
    test_clamp();
    test_release_is_honored_by_every_provider();
    T_REPORT();
}
