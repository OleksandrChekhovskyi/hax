/* SPDX-License-Identifier: MIT */
#include "text/diff.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "xalloc.h"
#include "text/utf8_sanitize.h"

#define CONTEXT_LINES 3

/* Lines materialized beyond the changed byte range, giving hunks their context and giving
 * slide_runs() room to move a run to a readable position. */
#define WINDOW_SLACK_LINES 64

/* Bounds for the divide-and-conquer Myers search. A region's step budget grows with its size up
 * to the cap; when a split exhausts it, the region divides at the furthest point reached, so only
 * that region's alignment degrades. The work limit bounds total line comparisons across the whole
 * diff; once it is spent, remaining regions are emitted as replacements. */
#define REGION_STEPS_MIN 64
#define REGION_STEPS_MAX 1024
#define WORK_LIMIT       ((size_t)1 << 28)

/* One side of the diff: the lines of that file's window. Offsets are 32-bit to keep per-line
 * metadata small; make_unified_diff() falls back to a whole-window replacement for the absurd
 * windows that would not fit. */
struct side {
    const char *data; /* window bytes, borrowed from the input buffer */
    uint32_t *off;    /* line start offsets; off[count] ends the window */
    uint32_t *hash;
    size_t count;
};

static const char *line_ptr(const struct side *side, size_t i)
{
    return side->data + side->off[i];
}

static uint32_t line_len(const struct side *side, size_t i)
{
    return side->off[i + 1] - side->off[i];
}

/* Lines compare including the trailing newline, so a final line without one never equals its
 * newline-terminated spelling and the '\ No newline at end of file' marker falls out of ordinary
 * line changes. */
static int lines_eq(const struct side *x, size_t i, const struct side *y, size_t j)
{
    uint32_t len = line_len(x, i);
    return x->hash[i] == y->hash[j] && len == line_len(y, j) &&
           memcmp(line_ptr(x, i), line_ptr(y, j), len) == 0;
}

static int line_blank(const struct side *side, size_t i)
{
    const char *ptr = line_ptr(side, i);
    uint32_t len = line_len(side, i);
    for (uint32_t at = 0; at < len; at++) {
        char c = ptr[at];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return 0;
    }
    return 1;
}

static void side_init(struct side *side, const char *data, size_t len)
{
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            count++;
    }
    if (len > 0 && data[len - 1] != '\n')
        count++;

    side->data = data;
    side->count = count;
    side->off = xmalloc((count + 1) * sizeof(*side->off));
    side->hash = xmalloc((count + 1) * sizeof(*side->hash));

    size_t at = 1;
    side->off[0] = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == '\n')
            side->off[at++] = (uint32_t)(i + 1);
    }
    side->off[count] = (uint32_t)len;

    for (size_t l = 0; l < count; l++) {
        uint32_t hash = 2166136261u; /* FNV-1a */
        for (uint32_t i = side->off[l]; i < side->off[l + 1]; i++) {
            hash ^= (unsigned char)data[i];
            hash *= 16777619u;
        }
        side->hash[l] = hash;
    }
}

static void side_free(struct side *side)
{
    free(side->off);
    free(side->hash);
}

/* The byte range both diff sides materialize lines for: the changed region extended by
 * WINDOW_SLACK_LINES whole lines on each end. Everything before lo is a shared prefix and
 * everything after {a,b}_hi a shared suffix, so per-line work stays proportional to the edit,
 * not the file. */
struct window {
    size_t lo; /* same offset in both files: it lies within the shared prefix */
    size_t a_hi;
    size_t b_hi;
    size_t base; /* lines preceding lo */
};

static struct window find_window(const char *a, size_t a_len, const char *b, size_t b_len)
{
    struct window w;
    size_t common = a_len < b_len ? a_len : b_len;
    size_t p = 0;
    while (p < common && a[p] == b[p])
        p++;

    size_t lo = p;
    while (lo > 0 && a[lo - 1] != '\n')
        lo--;
    for (int i = 0; i < WINDOW_SLACK_LINES && lo > 0; i++) {
        lo--;
        while (lo > 0 && a[lo - 1] != '\n')
            lo--;
    }
    w.lo = lo;
    w.base = 0;
    for (const char *q = a; (q = memchr(q, '\n', (size_t)(a + lo - q))) != NULL; q++)
        w.base++;

    size_t s = 0;
    size_t s_max = common - lo;
    while (s < s_max && a[a_len - 1 - s] == b[b_len - 1 - s])
        s++;
    /* Only line starts strictly inside the common suffix are boundaries in both files; the byte
     * before the suffix differs between them. A partial-line suffix trims nothing. */
    const char *nl = s ? memchr(a + (a_len - s), '\n', s) : NULL;
    if (!nl) {
        w.a_hi = a_len;
        w.b_hi = b_len;
        return w;
    }
    size_t hi = (size_t)(nl + 1 - a);
    for (int i = 0; i < WINDOW_SLACK_LINES && hi < a_len; i++) {
        const char *end = memchr(a + hi, '\n', a_len - hi);
        hi = end ? (size_t)(end + 1 - a) : a_len;
    }
    w.a_hi = hi;
    w.b_hi = b_len - (a_len - hi);
    return w;
}

struct dctx {
    const struct side *a, *b;
    unsigned char *a_changed, *b_changed;
    long *kvf, *kvb; /* forward/backward furthest-x per diagonal, indexed [kv_off + k] */
    long kv_off;
    size_t work; /* remaining line-comparison budget for the whole diff */
};

/* Treating lines as different once the work budget is spent stays correct — matches only shrink,
 * so affected lines are emitted as delete-then-insert pairs — it just makes the diff coarser. */
static int dctx_eq(struct dctx *ctx, size_t ai, size_t bi)
{
    if (ctx->work == 0)
        return 0;
    ctx->work--;
    return lines_eq(ctx->a, ai, ctx->b, bi);
}

static int dctx_eq_rev(struct dctx *ctx, size_t a_hi, size_t b_hi, long x, long y)
{
    return dctx_eq(ctx, a_hi - 1 - (size_t)x, b_hi - 1 - (size_t)y);
}

/* Diagonals hugging the region corners can push furthest-reach values past the region ("virtual"
 * points off the grid, as in the paper's basic form). Any interior point is a valid split — the
 * halves are diffed independently — so clamping is enough to keep the recursion sound. */
static void split_out(size_t a_lo, size_t b_lo, long n, long m, long x, long y, size_t *split_a,
                      size_t *split_b)
{
    if (x < 0)
        x = 0;
    if (x > n)
        x = n;
    if (y < 0)
        y = 0;
    if (y > m)
        y = m;
    *split_a = a_lo + (size_t)x;
    *split_b = b_lo + (size_t)y;
}

/* Find a split point of the region with the bidirectional furthest-reaching search from Myers,
 * "An O(ND) Difference Algorithm and Its Variations" (1986): forward and backward scans meet in
 * the middle in linear space. When the step budget runs out first, the region instead divides at
 * the furthest forward point reached, so an over-budget region only degrades locally around that
 * point. Any interior split yields a correct diff; where it lands affects quality alone. */
static void myers_split(struct dctx *ctx, size_t a_lo, size_t a_hi, size_t b_lo, size_t b_hi,
                        size_t *split_a, size_t *split_b)
{
    long n = (long)(a_hi - a_lo);
    long m = (long)(b_hi - b_lo);
    long delta = n - m;
    int odd = (int)(delta & 1);
    long off = ctx->kv_off;
    long *kvf = ctx->kvf;
    long *kvb = ctx->kvb;

    long budget = REGION_STEPS_MIN;
    while (budget < REGION_STEPS_MAX && budget * budget < n + m)
        budget *= 2;
    /* The scans always meet by half the region perimeter. */
    if (budget > (n + m) / 2 + 1)
        budget = (n + m) / 2 + 1;

    long best = -1;
    long best_x = 0;
    long best_y = 0;
    kvf[off + 1] = 0;
    kvb[off + 1] = 0;

    for (long d = 0; d <= budget && ctx->work > 0; d++) {
        for (long k = -d; k <= d; k += 2) {
            long x;
            if (k == -d || (k != d && kvf[off + k - 1] < kvf[off + k + 1]))
                x = kvf[off + k + 1];
            else
                x = kvf[off + k - 1] + 1;
            long y = x - k;
            while (x < n && y < m && dctx_eq(ctx, a_lo + (size_t)x, b_lo + (size_t)y)) {
                x++;
                y++;
            }
            kvf[off + k] = x;
            if (x <= n && y <= m && x + y > best) {
                best = x + y;
                best_x = x;
                best_y = y;
            }
            long kr = delta - k;
            if (odd && kr >= -(d - 1) && kr <= d - 1 && x >= n - kvb[off + kr]) {
                split_out(a_lo, b_lo, n, m, x, y, split_a, split_b);
                return;
            }
        }
        for (long k = -d; k <= d; k += 2) {
            long x;
            if (k == -d || (k != d && kvb[off + k - 1] < kvb[off + k + 1]))
                x = kvb[off + k + 1];
            else
                x = kvb[off + k - 1] + 1;
            long y = x - k;
            while (x < n && y < m && dctx_eq_rev(ctx, a_hi, b_hi, x, y)) {
                x++;
                y++;
            }
            kvb[off + k] = x;
            long kf = delta - k;
            if (!odd && kf >= -d && kf <= d && kvf[off + kf] >= n - x) {
                split_out(a_lo, b_lo, n, m, n - x, m - y, split_a, split_b);
                return;
            }
        }
    }

    split_out(a_lo, b_lo, n, m, best_x, best_y, split_a, split_b);
}

/* Mark deleted lines of a and inserted lines of b for the region [a_lo, a_hi) x [b_lo, b_hi);
 * unmarked lines are common. Recurses on the smaller half of each split and iterates on the
 * larger, bounding stack depth logarithmically. */
static void diff_region(struct dctx *ctx, size_t a_lo, size_t a_hi, size_t b_lo, size_t b_hi)
{
    for (;;) {
        while (a_lo < a_hi && b_lo < b_hi && dctx_eq(ctx, a_lo, b_lo)) {
            a_lo++;
            b_lo++;
        }
        while (a_hi > a_lo && b_hi > b_lo && dctx_eq(ctx, a_hi - 1, b_hi - 1)) {
            a_hi--;
            b_hi--;
        }
        if (a_lo == a_hi) {
            memset(ctx->b_changed + b_lo, 1, b_hi - b_lo);
            return;
        }
        if (b_lo == b_hi) {
            memset(ctx->a_changed + a_lo, 1, a_hi - a_lo);
            return;
        }

        size_t split_a, split_b;
        int degenerate = 1;
        if (ctx->work > 0) {
            myers_split(ctx, a_lo, a_hi, b_lo, b_hi, &split_a, &split_b);
            degenerate =
                (split_a == a_lo && split_b == b_lo) || (split_a == a_hi && split_b == b_hi);
        }
        if (degenerate) {
            memset(ctx->a_changed + a_lo, 1, a_hi - a_lo);
            memset(ctx->b_changed + b_lo, 1, b_hi - b_lo);
            return;
        }

        if ((split_a - a_lo) + (split_b - b_lo) <= (a_hi - split_a) + (b_hi - split_b)) {
            diff_region(ctx, a_lo, split_a, b_lo, split_b);
            a_lo = split_a;
            b_lo = split_b;
        } else {
            diff_region(ctx, split_a, a_hi, split_b, b_hi);
            a_hi = split_a;
            b_hi = split_b;
        }
    }
}

static int run_score(const struct side *side, size_t start, size_t end)
{
    int score = 0;
    if (start == 0 || line_blank(side, start - 1))
        score += 2;
    if (end == side->count || line_blank(side, end - 1))
        score += 1;
    return score;
}

/* A run of changed lines bounded by equal lines can sit at several positions that all describe
 * the same edit; pick the most readable one so an inserted or deleted block aligns with the
 * blank-separated structure around it instead of starting mid-block. Runs starting right after a
 * blank line (or a file edge) score highest, runs whose last line is blank next, and ties settle
 * on the bottommost position, matching what diff(1) users expect. */
static void slide_runs(const struct side *side, unsigned char *changed)
{
    size_t count = side->count;
    size_t scan = 0;
    while (scan < count) {
        if (!changed[scan]) {
            scan++;
            continue;
        }
        size_t start = scan;
        size_t end = start;
        while (end < count && changed[end])
            end++;
        size_t run = end - start;

        size_t lo = start;
        while (lo > 0 && !changed[lo - 1] && lines_eq(side, lo - 1, side, lo + run - 1))
            lo--;
        size_t hi = start;
        while (hi + run < count && !changed[hi + run] && lines_eq(side, hi, side, hi + run))
            hi++;

        size_t best = lo;
        int best_score = -1;
        for (size_t pos = lo; pos <= hi; pos++) {
            int score = run_score(side, pos, pos + run);
            if (score >= best_score) {
                best_score = score;
                best = pos;
            }
        }
        if (best != start) {
            memset(changed + start, 0, run);
            memset(changed + best, 1, run);
        }
        scan = (best > start ? best : start) + run;
    }
}

struct change {
    size_t a_start, a_lines;
    size_t b_start, b_lines;
};

/* Walk both flag arrays in lockstep — unchanged lines pair up one to one — and record each
 * maximal deleted/inserted region with its position on both sides. */
static struct change *collect_changes(size_t n, size_t m, const unsigned char *a_changed,
                                      const unsigned char *b_changed, size_t *count_out)
{
    struct change *changes = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i = 0;
    size_t j = 0;
    while (i < n || j < m) {
        if ((i < n && a_changed[i]) || (j < m && b_changed[j])) {
            if (count == cap) {
                cap = cap ? cap * 2 : 8;
                changes = xrealloc(changes, cap * sizeof(*changes));
            }
            struct change *c = &changes[count++];
            c->a_start = i;
            c->b_start = j;
            while (i < n && a_changed[i])
                i++;
            while (j < m && b_changed[j])
                j++;
            c->a_lines = i - c->a_start;
            c->b_lines = j - c->b_start;
        } else {
            i++;
            j++;
        }
    }
    *count_out = count;
    return changes;
}

static void append_line(struct buf *out, char marker, const struct side *side, size_t i)
{
    buf_append(out, &marker, 1);
    buf_append(out, line_ptr(side, i), line_len(side, i));
    if (line_ptr(side, i)[line_len(side, i) - 1] != '\n')
        buf_append_str(out, "\n\\ No newline at end of file\n");
}

/* GNU convention: 1-based start with ",count" omitted when the count is 1; a zero-count side
 * names the line before the gap (0 at file start). */
static void append_range(struct buf *out, size_t start, size_t count)
{
    char text[48];
    if (count == 1)
        snprintf(text, sizeof(text), "%zu", start + 1);
    else
        snprintf(text, sizeof(text), "%zu,%zu", count ? start + 1 : start, count);
    buf_append_str(out, text);
}

static void append_hunks(struct buf *out, const struct side *a, const struct side *b,
                         const struct change *changes, size_t change_count, size_t base)
{
    size_t group = 0;
    while (group < change_count) {
        size_t last = group;
        while (last + 1 < change_count &&
               changes[last + 1].a_start - (changes[last].a_start + changes[last].a_lines) <=
                   2 * CONTEXT_LINES)
            last++;

        size_t a_end = changes[last].a_start + changes[last].a_lines;
        size_t b_end = changes[last].b_start + changes[last].b_lines;
        size_t a_lo =
            changes[group].a_start > CONTEXT_LINES ? changes[group].a_start - CONTEXT_LINES : 0;
        size_t a_hi = a_end + CONTEXT_LINES <= a->count ? a_end + CONTEXT_LINES : a->count;
        /* Context lines around a group are unchanged and pair up across the two sides, so the
         * b bounds follow from the a bounds. */
        size_t b_lo = changes[group].b_start - (changes[group].a_start - a_lo);
        size_t b_hi = b_end + (a_hi - a_end);

        buf_append_str(out, "@@ -");
        append_range(out, base + a_lo, a_hi - a_lo);
        buf_append_str(out, " +");
        append_range(out, base + b_lo, b_hi - b_lo);
        buf_append_str(out, " @@\n");

        size_t i = a_lo;
        for (size_t c = group; c <= last; c++) {
            for (; i < changes[c].a_start; i++)
                append_line(out, ' ', a, i);
            for (; i < changes[c].a_start + changes[c].a_lines; i++)
                append_line(out, '-', a, i);
            for (size_t j = changes[c].b_start; j < changes[c].b_start + changes[c].b_lines; j++)
                append_line(out, '+', b, j);
        }
        for (; i < a_hi; i++)
            append_line(out, ' ', a, i);

        group = last + 1;
    }
}

static size_t region_line_count(const char *data, size_t len)
{
    size_t count = 0;
    for (const char *q = data; (q = memchr(q, '\n', (size_t)(data + len - q))) != NULL; q++)
        count++;
    if (len > 0 && data[len - 1] != '\n')
        count++;
    return count;
}

static void append_region_lines(struct buf *out, char marker, const char *data, size_t len)
{
    size_t start = 0;
    while (start < len) {
        const char *nl = memchr(data + start, '\n', len - start);
        size_t end = nl ? (size_t)(nl - data) + 1 : len;
        buf_append(out, &marker, 1);
        buf_append(out, data + start, end - start);
        if (!nl)
            buf_append_str(out, "\n\\ No newline at end of file\n");
        start = end;
    }
}

char *make_unified_diff(const char *a_data, size_t a_len, const char *b_data, size_t b_len,
                        const char *a_label, const char *b_label)
{
    if (a_len == b_len && (a_len == 0 || memcmp(a_data, b_data, a_len) == 0))
        return xstrdup("");

    struct window w = find_window(a_data, a_len, b_data, b_len);
    size_t a_win = w.a_hi - w.lo;
    size_t b_win = w.b_hi - w.lo;

    struct buf out;
    buf_init(&out);
    buf_append_str(&out, "--- ");
    buf_append_str(&out, a_label);
    buf_append_str(&out, "\n+++ ");
    buf_append_str(&out, b_label);
    buf_append_str(&out, "\n");

    if (a_win > (size_t)UINT32_MAX || b_win > (size_t)UINT32_MAX) {
        /* Changed regions beyond 32-bit offsets get one replacement hunk without per-line
         * metadata; both callers cap inputs far below this. */
        buf_append_str(&out, "@@ -");
        append_range(&out, w.base, region_line_count(a_data + w.lo, a_win));
        buf_append_str(&out, " +");
        append_range(&out, w.base, region_line_count(b_data + w.lo, b_win));
        buf_append_str(&out, " @@\n");
        append_region_lines(&out, '-', a_data + w.lo, a_win);
        append_region_lines(&out, '+', b_data + w.lo, b_win);
    } else {
        struct side a, b;
        side_init(&a, a_data + w.lo, a_win);
        side_init(&b, b_data + w.lo, b_win);
        unsigned char *a_changed = xcalloc(a.count + 1, 1);
        unsigned char *b_changed = xcalloc(b.count + 1, 1);
        long *kv = xmalloc(2 * (2 * (size_t)REGION_STEPS_MAX + 3) * sizeof(*kv));
        struct dctx ctx = {
            .a = &a,
            .b = &b,
            .a_changed = a_changed,
            .b_changed = b_changed,
            .kvf = kv,
            .kvb = kv + 2 * REGION_STEPS_MAX + 3,
            .kv_off = REGION_STEPS_MAX + 1,
            .work = WORK_LIMIT,
        };
        diff_region(&ctx, 0, a.count, 0, b.count);
        free(kv);

        slide_runs(&a, a_changed);
        slide_runs(&b, b_changed);

        size_t change_count;
        struct change *changes =
            collect_changes(a.count, b.count, a_changed, b_changed, &change_count);
        append_hunks(&out, &a, &b, changes, change_count, w.base);

        free(changes);
        free(a_changed);
        free(b_changed);
        side_free(&a);
        side_free(&b);
    }

    /* The new side arrives as JSON text, but the old side copies raw bytes from disk into '-'
     * and context lines; sanitizing keeps NULs and invalid sequences out of the JSON-bound tool
     * result while preserving the line structure. */
    char *clean = utf8_sanitize(out.data, out.len);
    buf_free(&out);
    return clean;
}
