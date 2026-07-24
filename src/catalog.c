/* SPDX-License-Identifier: MIT */
#include "catalog.h"

#include <libgen.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <jansson.h>

#include "config.h"
#include "util.h"
#include "system/bg_job.h"
#include "system/fs.h"
#include "transport/http.h"

#define CATALOG_CACHE_FILE "catalog.json"
/* The full catalog is ~3 MB; give a slow link room, but a hung endpoint
 * must not pin the worker until process exit (shutdown cancels via the
 * bg tick anyway — this is the no-shutdown bound). */
#define CATALOG_FETCH_TIMEOUT_S 30
/* Bounds the downloaded body (enforced mid-transfer by http_get) and the
 * cache-file slurp. Memory scales linearly with it — the artifact is
 * never tree-parsed whole (see catalog_extract_member), so this is pure
 * buffer headroom: ~10x the current artifact. */
#define CATALOG_MAX_BYTES (32 * 1024 * 1024)
/* catalog_prefetch flags a snapshot older than this: refreshes have been
 * failing (endpoint moved, artifact outgrew CATALOG_MAX_BYTES, broken
 * proxy) for long enough that estimates may have drifted. Far past the
 * refresh TTL so transient outages never trip it. */
#define CATALOG_STALE_WARN_S (30L * 24 * 60 * 60)

/* ---------------- entry parsing (shared by both tiers) ---------------- */

static void entry_init(struct catalog_entry *e)
{
    e->cost_input = -1;
    e->cost_output = -1;
    e->cost_cache_read = -1;
    e->cost_cache_write = -1;
    e->context = 0;
    e->output = 0;
    e->image_input = -1;
    e->n_tiers = 0;
    e->tiers_declared = 0;
}

/* Member `k` of `obj` as a non-negative rate. Accepts a JSON number (the
 * raw models.dev artifact) or a numeric string (config values pass through
 * normalize(), which stringifies scalars). -1 = absent/invalid. */
static double member_rate(json_t *obj, const char *k)
{
    json_t *v = json_object_get(obj, k);
    if (json_is_number(v)) {
        double d = json_number_value(v);
        return d >= 0 ? d : -1;
    }
    const char *s = json_string_value(v);
    if (!s || !*s)
        return -1;
    char *end;
    double d = strtod(s, &end);
    return (end != s && !*end && d >= 0) ? d : -1;
}

/* Member `k` of `obj` as a token count. Accepts a JSON integer or a
 * parse_size string ("400000", "256k"). 0 = absent/invalid. */
static long member_tokens(json_t *obj, const char *k)
{
    json_t *v = json_object_get(obj, k);
    if (json_is_integer(v)) {
        long n = (long)json_integer_value(v);
        return n > 0 ? n : 0;
    }
    return parse_size(json_string_value(v));
}

/* Take a `cost.tiers` array into *e whole (see the whole-list rule in
 * catalog.h). Elements that aren't context tiers with a positive
 * threshold are skipped; anything past CATALOG_TIERS_MAX is dropped. */
static void tiers_fill(json_t *tiers, struct catalog_entry *e)
{
    if (e->tiers_declared || !json_is_array(tiers))
        return;
    e->tiers_declared = 1; /* an empty array declares "flat-priced" */
    size_t i;
    json_t *tv;
    json_array_foreach(tiers, i, tv)
    {
        if (e->n_tiers >= CATALOG_TIERS_MAX)
            break;
        if (!json_is_object(tv))
            continue;
        json_t *sel = json_object_get(tv, "tier");
        if (!json_is_object(sel))
            continue;
        /* Strictly require type == "context": a missing or mistyped
         * selector must fail toward the declared flat rates, not toward
         * a surprise long-context surcharge. */
        const char *type = json_string_value(json_object_get(sel, "type"));
        if (!type || strcmp(type, "context") != 0)
            continue;
        long above = member_tokens(sel, "size");
        if (above <= 0)
            continue;
        struct catalog_tier *t = &e->tiers[e->n_tiers++];
        t->above = above;
        t->cost_input = member_rate(tv, "input");
        t->cost_output = member_rate(tv, "output");
        t->cost_cache_read = member_rate(tv, "cache_read");
        t->cost_cache_write = member_rate(tv, "cache_write");
    }
}

/* Fill the still-unknown fields of *e from a per-model object of the
 * models.dev shape ({"cost": {...}, "limit": {...}}). Known fields are
 * left alone, so a higher tier's values survive a lower tier's pass. */
static void entry_fill(json_t *model_obj, struct catalog_entry *e)
{
    if (!json_is_object(model_obj))
        return;
    json_t *cost = json_object_get(model_obj, "cost");
    if (json_is_object(cost)) {
        if (e->cost_input < 0)
            e->cost_input = member_rate(cost, "input");
        if (e->cost_output < 0)
            e->cost_output = member_rate(cost, "output");
        if (e->cost_cache_read < 0)
            e->cost_cache_read = member_rate(cost, "cache_read");
        if (e->cost_cache_write < 0)
            e->cost_cache_write = member_rate(cost, "cache_write");
        tiers_fill(json_object_get(cost, "tiers"), e);
    }
    json_t *limit = json_object_get(model_obj, "limit");
    if (json_is_object(limit)) {
        if (e->context <= 0)
            e->context = member_tokens(limit, "context");
        if (e->output <= 0)
            e->output = member_tokens(limit, "output");
    }
    if (e->image_input < 0) {
        json_t *modalities = json_object_get(model_obj, "modalities");
        json_t *input = json_is_object(modalities) ? json_object_get(modalities, "input") : NULL;
        if (json_is_array(input)) {
            e->image_input = 0;
            size_t i;
            json_t *m;
            json_array_foreach(input, i, m)
            {
                const char *s = json_string_value(m);
                if (s && strcmp(s, "image") == 0)
                    e->image_input = 1;
            }
        }
    }
}

static int entry_complete(const struct catalog_entry *e)
{
    /* Tiers count toward completeness via the *declared* flag: a config
     * block that pins every scalar but says nothing about tiers must
     * still fall through to the cache, or a tiered model would silently
     * price flat (the memoized lookup keeps that consult cheap). */
    return e->cost_input >= 0 && e->cost_output >= 0 && e->cost_cache_read >= 0 &&
           e->cost_cache_write >= 0 && e->context > 0 && e->output > 0 && e->image_input >= 0 &&
           e->tiers_declared;
}

static int entry_any(const struct catalog_entry *e)
{
    /* Tiers count as resolved metadata: a tier-only entry (custom model
     * declaring just its long-context rates) is priceable above its
     * threshold, so it must not read as "unknown model". */
    return e->cost_input >= 0 || e->cost_output >= 0 || e->cost_cache_read >= 0 ||
           e->cost_cache_write >= 0 || e->context > 0 || e->output > 0 || e->image_input >= 0 ||
           e->n_tiers > 0;
}

/* Fill the still-unknown fields of *dst from *src — the struct-to-struct
 * twin of entry_fill, used to overlay the config tier on a memoized
 * cache-tier entry. */
static void entry_merge(struct catalog_entry *dst, const struct catalog_entry *src)
{
    if (dst->cost_input < 0)
        dst->cost_input = src->cost_input;
    if (dst->cost_output < 0)
        dst->cost_output = src->cost_output;
    if (dst->cost_cache_read < 0)
        dst->cost_cache_read = src->cost_cache_read;
    if (dst->cost_cache_write < 0)
        dst->cost_cache_write = src->cost_cache_write;
    if (dst->context <= 0)
        dst->context = src->context;
    if (dst->output <= 0)
        dst->output = src->output;
    if (dst->image_input < 0)
        dst->image_input = src->image_input;
    if (!dst->tiers_declared && src->tiers_declared) {
        memcpy(dst->tiers, src->tiers, sizeof(dst->tiers));
        dst->n_tiers = src->n_tiers;
        dst->tiers_declared = 1;
    }
}

/* ---------------- top-level member extraction ---------------- */

/* The artifact is multi-MB and jansson inflates JSON ~10x when building a
 * tree, so nothing here ever tree-parses the whole file. A byte-level scan
 * locates one top-level member's value span, and only that slice (~100 KB
 * for a large provider) is handed to jansson — which also supplies the
 * real validation the scan doesn't do. Bytewise scanning is UTF-8-safe:
 * '"' and '\\' never occur inside multi-byte sequences. */

static const char *scan_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Advance past the string whose opening '"' is at `p`. Returns the
 * position just past the closing quote, NULL on truncated input. */
static const char *scan_string(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!p[1])
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Advance past one JSON value starting at `p`: strings and {} / []
 * nesting are honored, everything else is structural-only. NULL on
 * truncated input. */
static const char *scan_value(const char *p)
{
    if (*p == '"')
        return scan_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = scan_string(p);
                if (!p)
                    return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }
    /* Scalar token (number / true / false / null): up to a delimiter. */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' &&
           *p != '\r')
        p++;
    return p;
}

/* One step of top-level member iteration. On entry *pp points at a member
 * key's opening quote; on success fills the key and value spans and
 * advances *pp — to the next member's key (returns 0), or just past the
 * root's closing brace when this was the last member (returns 1, leaving
 * *pp at the trailing bytes for an EOF check). Returns -1 on malformed or
 * truncated input. */
static int scan_member(const char **pp, const char **kstart, size_t *klen, const char **vstart,
                       const char **vend)
{
    const char *p = *pp;
    if (*p != '"')
        return -1;
    *kstart = p + 1;
    const char *kend = scan_string(p);
    if (!kend)
        return -1;
    *klen = (size_t)(kend - 1 - *kstart);
    p = scan_ws(kend);
    if (*p != ':')
        return -1;
    p = scan_ws(p + 1);
    *vstart = p;
    p = scan_value(p);
    if (!p)
        return -1;
    *vend = p;
    p = scan_ws(p);
    if (*p == ',') {
        *pp = scan_ws(p + 1);
        return 0;
    }
    if (*p == '}') {
        *pp = p + 1; /* that was the last member */
        return 1;
    }
    return -1;
}

/* Position of the first member key in object `text`; NULL for an empty
 * object or a non-object. */
static const char *scan_first_member(const char *text)
{
    const char *p = scan_ws(text);
    if (*p != '{')
        return NULL;
    p = scan_ws(p + 1);
    return *p == '"' ? p : NULL;
}

json_t *catalog_extract_member(const char *text, const char *key)
{
    if (!text || !key || !*key)
        return NULL;
    size_t key_len = strlen(key);
    const char *p = scan_first_member(text);
    if (!p)
        return NULL;
    for (;;) {
        const char *kstart, *vstart, *vend;
        size_t klen;
        int last = scan_member(&p, &kstart, &klen, &vstart, &vend);
        if (last < 0)
            return NULL;
        if (klen == key_len && memcmp(kstart, key, key_len) == 0)
            return json_loadb(vstart, (size_t)(vend - vstart), JSON_DECODE_ANY, NULL);
        if (last)
            return NULL; /* members exhausted */
    }
}

/* Accept a body as a catalog: some top-level member must be an object
 * carrying a `models` object, every member's value must be valid JSON,
 * and nothing but whitespace may follow the root's closing brace. The
 * first condition rejects JSON-shaped error payloads ({"error": "rate
 * limited"} behind a broken proxy); the others reject truncated or
 * corrupted tails — replacing the previous (complete) snapshot with a
 * file with holes would silently lose providers, and the fresh mtime
 * would suppress a recovering re-fetch for a whole refresh interval.
 * Validation is piecewise — one member slice parsed at a time, never the
 * whole artifact — so peak memory stays one slice; the full-parse CPU
 * cost lands on the background fetch worker where it doesn't matter. */
static int catalog_text_valid(const char *text)
{
    int ok = 0;
    const char *p = scan_first_member(text);
    if (!p)
        return 0;
    for (;;) {
        const char *kstart, *vstart, *vend;
        size_t klen;
        int last = scan_member(&p, &kstart, &klen, &vstart, &vend);
        if (last < 0)
            return 0;
        json_t *v = json_loadb(vstart, (size_t)(vend - vstart), JSON_DECODE_ANY, NULL);
        if (!v)
            return 0; /* brace-balanced garbage the structural scan waved through */
        if (!ok)
            ok = json_is_object(v) && json_is_object(json_object_get(v, "models"));
        json_decref(v);
        if (last)
            break;
    }
    return ok && *scan_ws(p) == '\0';
}

/* ---------------- config tier: the catalog.models block ---------------- */

static void fill_from_config(const char *provider_id, const char *model, struct catalog_entry *e)
{
    const json_t *models = config_json_node("catalog.models");
    if (!json_is_object(models))
        return;
    json_t *prov = json_object_get((json_t *)models, provider_id);
    if (!json_is_object(prov))
        return;
    entry_fill(json_object_get(prov, model), e);
}

/* ---------------- cache tier: the fetched snapshot ---------------- */

/* Slurp the cached artifact and tree-parse only `provider_id`'s slice — a
 * few ms and one slice's worth of memory. New reference, or NULL when the
 * cache is missing, over the size cap, or has no such provider. */
static json_t *cache_provider_slice(const char *provider_id)
{
    char *path = xdg_hax_cache_path(CATALOG_CACHE_FILE);
    if (!path)
        return NULL;
    size_t len;
    int truncated;
    char *text = slurp_file_capped(path, CATALOG_MAX_BYTES, &len, &truncated);
    free(path);
    if (!text)
        return NULL;
    json_t *prov = truncated ? NULL : catalog_extract_member(text, provider_id);
    free(text);
    return prov;
}

/* Read one model out of an already-extracted provider slice. */
static void fill_from_slice(const json_t *prov, const char *model, struct catalog_entry *e)
{
    json_t *models = prov ? json_object_get(prov, "models") : NULL;
    if (json_is_object(models))
        entry_fill(json_object_get(models, model), e);
}

/* Extract the wanted provider's slice and read one model out of it; the
 * memo in cache_tier_lookup bounds repeats. Callers resolving many models
 * of one provider at once should use catalog_lookup_many instead, which
 * pays the slurp + slice-parse once for the whole batch. */
static void fill_from_cache(const char *provider_id, const char *model, struct catalog_entry *e)
{
    json_t *prov = cache_provider_slice(provider_id);
    if (!prov)
        return;
    fill_from_slice(prov, model, e);
    json_decref(prov);
}

/* ---------------- cache-tier memo (foreground thread) ---------------- */

/* The memo holds *cache-tier* entries — the slurp + scan + slice-parse is
 * a few ms, worth remembering per (provider, model); the config tier is a
 * tiny-JSON walk redone on every catalog_lookup. Foreground-only: the one
 * background actor is the fetch worker, which touches nothing but the
 * cache file and the atomic generation counter below. */

struct memo {
    char *provider_id;
    char *model;
    struct catalog_entry entry;
    int found;
};

static struct memo *g_memo;
static size_t g_n_memo, g_cap_memo;

/* Bumped by the fetch worker when a fresh snapshot lands; synced on lookup
 * so memoized misses don't outlive the refresh that could turn them into
 * hits. */
static _Atomic int g_cache_gen;
static int g_memo_gen;

static void memo_clear(void)
{
    for (size_t i = 0; i < g_n_memo; i++) {
        free(g_memo[i].provider_id);
        free(g_memo[i].model);
    }
    free(g_memo);
    g_memo = NULL;
    g_n_memo = g_cap_memo = 0;
}

static struct memo *memo_find(const char *provider_id, const char *model)
{
    for (size_t i = 0; i < g_n_memo; i++)
        if (strcmp(g_memo[i].provider_id, provider_id) == 0 && strcmp(g_memo[i].model, model) == 0)
            return &g_memo[i];
    return NULL;
}

static void memo_add(const char *provider_id, const char *model, const struct catalog_entry *e,
                     int found)
{
    if (g_n_memo == g_cap_memo) {
        g_cap_memo = g_cap_memo ? g_cap_memo * 2 : 4;
        g_memo = xrealloc(g_memo, g_cap_memo * sizeof(*g_memo));
    }
    struct memo *m = &g_memo[g_n_memo++];
    m->provider_id = xstrdup(provider_id);
    m->model = xstrdup(model);
    m->entry = *e;
    m->found = found;
}

static int cache_tier_lookup(const char *provider_id, const char *model, struct catalog_entry *out)
{
    int gen = atomic_load(&g_cache_gen);
    if (gen != g_memo_gen) {
        memo_clear();
        g_memo_gen = gen;
    }
    struct memo *m = memo_find(provider_id, model);
    if (m) {
        *out = m->entry;
        return m->found ? 0 : -1;
    }
    entry_init(out);
    fill_from_cache(provider_id, model, out);
    int found = entry_any(out);
    memo_add(provider_id, model, out, found);
    return found ? 0 : -1;
}

/* ---------------- tier resolution ---------------- */

/* Where one resolve reads its cache tier from. The two entry points differ
 * only here: a single lookup pays the memoized per-model slurp+scan, while a
 * batch extracts one provider slice up front and points every model at it.
 *
 * The indirection is what keeps "NULL means fall back to the memo" from ever
 * being expressible: a batch whose slurp failed passes no cache tier at all,
 * rather than silently degrading into the per-model reads that
 * catalog_lookup_many exists to avoid. */
struct cache_tier {
    int (*fill)(const struct cache_tier *t, const char *model, struct catalog_entry *out);
    const char *provider_id;
    const json_t *slice;
};

static int cache_tier_memo(const struct cache_tier *t, const char *model, struct catalog_entry *out)
{
    return cache_tier_lookup(t->provider_id, model, out) == 0;
}

static int cache_tier_slice(const struct cache_tier *t, const char *model,
                            struct catalog_entry *out)
{
    entry_init(out);
    fill_from_slice(t->slice, model, out);
    return entry_any(out);
}

/* The tier policy, in one place so the single and batch entry points cannot
 * drift (catalog.h promises callers they agree): config wins field by field,
 * the cache tier is consulted only for what config left undeclared and only
 * fills the gaps, and a resolve counts as a hit when anything at all landed.
 * A NULL `cache` resolves from config alone. Returns 1 on a hit. */
static int entry_resolve(const char *provider_id, const char *model, const struct cache_tier *cache,
                         struct catalog_entry *out)
{
    entry_init(out);
    if (!provider_id || !*provider_id || !model || !*model)
        return 0;
    fill_from_config(provider_id, model, out);
    struct catalog_entry cached;
    if (!entry_complete(out) && cache && cache->fill(cache, model, &cached))
        entry_merge(out, &cached);
    return entry_any(out);
}

int catalog_lookup(const char *provider_id, const char *model, struct catalog_entry *out)
{
    struct cache_tier memo = {.fill = cache_tier_memo, .provider_id = provider_id};
    return entry_resolve(provider_id, model, &memo, out) ? 0 : -1;
}

void catalog_lookup_many(const char *provider_id, const char *const *models, size_t n,
                         struct catalog_entry *out, int *found)
{
    for (size_t i = 0; i < n; i++) {
        entry_init(&out[i]);
        if (found)
            found[i] = 0;
    }
    if (!provider_id || !*provider_id || n == 0)
        return;

    /* The one slurp + slice-parse this entry point exists for. A miss (no
     * cache file, provider absent from the snapshot) leaves every model to
     * the config tier alone — deliberately not a per-model retry. */
    json_t *prov = cache_provider_slice(provider_id);
    struct cache_tier slice = {.fill = cache_tier_slice, .provider_id = provider_id, .slice = prov};
    for (size_t i = 0; i < n; i++) {
        int hit = entry_resolve(provider_id, models[i], prov ? &slice : NULL, &out[i]);
        if (found)
            found[i] = hit;
    }
    json_decref(prov);
}

double catalog_price(const struct catalog_entry *e, long input, long output, long cached,
                     long cache_write, long cache_write_1h, struct catalog_split *split)
{
    if (split)
        *split = (struct catalog_split){0};

    /* Tier selection: the request's total input (cache subsets included)
     * picks the highest threshold it exceeds; the whole request bills at
     * that tier's rates, with the tier's undeclared fields falling back
     * to the base rates. */
    double r_in = e->cost_input, r_out = e->cost_output;
    double r_read = e->cost_cache_read, r_write = e->cost_cache_write;
    long matched = -1;
    for (int i = 0; i < e->n_tiers; i++) {
        const struct catalog_tier *t = &e->tiers[i];
        if (input <= t->above || t->above <= matched)
            continue;
        matched = t->above;
        r_in = t->cost_input >= 0 ? t->cost_input : e->cost_input;
        r_out = t->cost_output >= 0 ? t->cost_output : e->cost_output;
        r_read = t->cost_cache_read >= 0 ? t->cost_cache_read : e->cost_cache_read;
        r_write = t->cost_cache_write >= 0 ? t->cost_cache_write : e->cost_cache_write;
    }
    if (r_in < 0 || r_out < 0)
        return -1;

    long cr = cached > 0 ? cached : 0;
    long cw = cache_write > 0 ? cache_write : 0;
    long cw1h = cache_write_1h > 0 ? cache_write_1h : 0;
    if (cw1h > cw)
        cw1h = cw; /* defensive: contract says 1h writes are a subset */
    long in = input > 0 ? input : 0;
    long uncached = in - cr - cw;
    if (uncached < 0)
        uncached = 0;
    if (r_read < 0)
        r_read = r_in;
    if (r_write < 0)
        r_write = r_in;
    double c_in = (double)uncached * r_in / 1e6;
    double c_read = (double)cr * r_read / 1e6;
    /* 1h cache writes bill at 2x input (see the header contract); only
     * the remaining (5m) writes take the catalog's cache_write rate. */
    double c_write = ((double)(cw - cw1h) * r_write + (double)cw1h * 2 * r_in) / 1e6;
    double c_out = (output > 0 ? (double)output : 0) * r_out / 1e6;
    if (split) {
        split->in = c_in;
        split->cache_read = c_read;
        split->cache_write = c_write;
        split->out = c_out;
    }
    return c_in + c_read + c_write + c_out;
}

/* ---------------- background fetch ---------------- */

static struct bg_job *g_fetch;
static int g_prefetch_ran;
/* Set by the worker on every exit path; lets catalog_drain poll for
 * completion without a timed-join primitive. */
static _Atomic int g_fetch_done;

struct fetch_args {
    char *url;
    char *path;
};

static void fetch_args_free(struct fetch_args *a)
{
    if (!a)
        return;
    free(a->url);
    free(a->path);
    free(a);
}

/* Stage the body in a sibling temp file and rename() it into place, so a
 * concurrent lookup (this process or another) never sees a torn file. */
static int write_cache_atomic(const char *path, const char *body, size_t len)
{
    char *dup = xstrdup(path);
    fs_mkdir_p(dirname(dup));
    free(dup);

    char *tmp = xasprintf("%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    int rc = write_all(fd, body, len);
    if (close(fd) != 0)
        rc = -1;
    if (rc == 0 && rename(tmp, path) != 0)
        rc = -1;
    if (rc != 0)
        unlink(tmp);
    free(tmp);
    return rc;
}

static void fetch_run(struct bg_job *job, void *arg)
{
    struct fetch_args *a = arg;
    if (!bg_job_cancelled(job)) {
        char *body = NULL;
        if (http_get(a->url, NULL, CATALOG_FETCH_TIMEOUT_S, CATALOG_MAX_BYTES, bg_job_tick, job,
                     &body, NULL) == 0 &&
            body) {
            if (catalog_text_valid(body) && write_cache_atomic(a->path, body, strlen(body)) == 0)
                atomic_fetch_add(&g_cache_gen, 1);
        }
        free(body);
    }
    fetch_args_free(a);
    atomic_store(&g_fetch_done, 1);
}

long catalog_prefetch(void)
{
    if (g_prefetch_ran)
        return 0;
    g_prefetch_ran = 1; /* one attempt per run, even on early-outs below */

    const char *url = config_str("catalog.url");
    if (!url || !*url)
        return 0; /* explicit empty = no fetching — and no staleness alarm:
                     the user opted out of refreshes */
    long ttl_ms = config_duration_ms("catalog.refresh");
    if (ttl_ms <= 0)
        return 0; /* 0 disables refresh */
    char *path = xdg_hax_cache_path(CATALOG_CACHE_FILE);
    if (!path)
        return 0;

    long stale_days = 0;
    struct stat st;
    if (stat(path, &st) == 0) {
        long age_s = (long)(time(NULL) - st.st_mtime);
        if (age_s < ttl_ms / 1000) {
            free(path);
            return 0; /* fresh enough */
        }
        if (age_s > CATALOG_STALE_WARN_S)
            stale_days = age_s / (24L * 60 * 60);
    }

    struct fetch_args *a = xcalloc(1, sizeof(*a));
    a->url = xstrdup(url);
    a->path = path;
    g_fetch = bg_job_spawn(fetch_run, a);
    if (!g_fetch)
        fetch_args_free(a); /* worker's free path never runs on spawn failure */
    return stale_days;
}

void catalog_drain(long max_wait_ms)
{
    if (!g_fetch)
        return;
    for (long waited = 0; waited < max_wait_ms && !atomic_load(&g_fetch_done); waited += 20) {
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    /* Finished or out of patience — settle the handle either way. A join
     * after the done flag is momentary; a timed-out fetch gets the same
     * cancel+join shutdown would give it. */
    if (!atomic_load(&g_fetch_done))
        bg_job_cancel(g_fetch);
    bg_job_join(g_fetch);
    g_fetch = NULL;
}

void catalog_shutdown(void)
{
    if (g_fetch) {
        bg_job_cancel(g_fetch);
        bg_job_join(g_fetch);
        g_fetch = NULL;
    }
    memo_clear();
    g_memo_gen = atomic_load(&g_cache_gen);
}
