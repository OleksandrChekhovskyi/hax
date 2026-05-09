/* SPDX-License-Identifier: MIT */
#include "util.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "provider.h"
#include "utf8.h"

static int locale_utf8 = 0;

void locale_init_utf8(void)
{
    setlocale(LC_CTYPE, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") == 0) {
        locale_utf8 = 1;
        return;
    }
    if (setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "en_US.UTF-8"))
        locale_utf8 = 1;
}

int locale_have_utf8(void)
{
    return locale_utf8;
}

static void oom(void)
{
    fprintf(stderr, "hax: out of memory\n");
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n)
        oom();
    return q;
}

char *xstrdup(const char *s)
{
    if (!s)
        return NULL;
    char *r = strdup(s);
    if (!r)
        oom();
    return r;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return NULL;
    }
    char *p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

void gen_uuid_v4(char out[37])
{
    uint8_t b[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "hax: open /dev/urandom: %s\n", strerror(errno));
        abort();
    }
    size_t got = 0;
    while (got < sizeof(b)) {
        ssize_t r = read(fd, b + got, sizeof(b) - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "hax: read /dev/urandom: %s\n", strerror(errno));
            abort();
        }
        if (r == 0) {
            fprintf(stderr, "hax: unexpected EOF on /dev/urandom\n");
            abort();
        }
        got += (size_t)r;
    }
    close(fd);

    b[6] = (b[6] & 0x0f) | 0x40; /* RFC 4122 version 4 */
    b[8] = (b[8] & 0x3f) | 0x80; /* RFC 4122 variant */

    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],
             b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
}

int term_width(void)
{
    struct winsize ws;
    int w = 120;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        w = ws.ws_col;
    if (w < 40)
        w = 40;
    if (w > 200)
        w = 200;
    return w;
}

int display_width(void)
{
    int w = term_width();
    return w < DISPLAY_WIDTH_CAP ? w : DISPLAY_WIDTH_CAP;
}

/* Width of the codepoint at s[i..] in cells. Sets *consumed to the
 * codepoint's byte length. Substitutes a 1-cell width for control /
 * malformed / "dangerous" codepoints so a non-printable can't make
 * the running total go negative. Caller is responsible for ensuring
 * i < len; utf8_codepoint_cells guarantees *consumed >= 1 in that
 * case. */
static size_t cells_at(const char *s, size_t len, size_t i, size_t *consumed)
{
    int w = utf8_codepoint_cells(s, len, i, consumed);
    if (w < 0)
        w = 1;
    return (size_t)w;
}

/* Skip past any zero-width codepoints (combining marks, etc.)
 * starting at `i`. They ride visually on the prior glyph and must
 * stay attached to it across a cut or wrap boundary — without this,
 * a string ending in a combining mark would mis-render after
 * truncation, and a hard wrap immediately before a combining mark
 * would orphan the mark on the next row. */
static size_t skip_zero_width(const char *s, size_t len, size_t i)
{
    while (i < len) {
        size_t consumed;
        size_t w = cells_at(s, len, i, &consumed);
        if (w != 0)
            break;
        i += consumed;
    }
    return i;
}

/* Walk forward until `target_cells` cells of content have been
 * consumed (or end-of-buffer), returning the resulting byte position.
 * Trailing zero-width codepoints are absorbed so the cut point doesn't
 * orphan a combining mark from its base character. */
static size_t advance_cells(const char *s, size_t len, size_t target_cells)
{
    size_t i = 0;
    size_t cells = 0;
    while (i < len && cells < target_cells) {
        size_t consumed;
        size_t w = cells_at(s, len, i, &consumed);
        if (cells + w > target_cells)
            break;
        cells += w;
        i += consumed;
    }
    return skip_zero_width(s, len, i);
}

char *truncate_for_display(const char *s, size_t cap)
{
    if (!s)
        return xstrdup("");
    size_t n = strlen(s);
    /* Cheap fast path: byte length <= cap implies cell count <= cap
     * (each cell takes >= 1 byte in UTF-8). The reverse isn't true
     * (multi-byte codepoints make n > cap possible while cells <=
     * cap), so the in-budget check below covers the rest. */
    if (n <= cap)
        return xstrdup(s);
    /* If walking exactly `cap` cells reaches end-of-buffer, the whole
     * string fits — no truncation needed. */
    if (advance_cells(s, n, cap) == n)
        return xstrdup(s);
    /* Below 4 cells we can't fit a meaningful "..." marker — hard cut
     * at the codepoint boundary. Callers normally clamp budgets above
     * this floor; the branch is here so pathological inputs don't
     * underflow. */
    if (cap < 4) {
        size_t cut = advance_cells(s, n, cap);
        char *out = xmalloc(cut + 1);
        memcpy(out, s, cut);
        out[cut] = '\0';
        return out;
    }
    size_t cut = advance_cells(s, n, cap - 3);
    char *out = xmalloc(cut + 4);
    memcpy(out, s, cut);
    memcpy(out + cut, "...", 3);
    out[cut + 3] = '\0';
    return out;
}

/* Strict word-boundary break: rightmost-ASCII-space-within-budget,
 * else hard cut at max_cells cells. Returns 0 if even the first
 * codepoint exceeds the budget — wrap_break_pos adds a forward-
 * progress fallback on top for the wrap case, but the truncate path
 * in reflow_for_display calls this directly so an appended "..."
 * doesn't push the row past its width. */
static size_t strict_break_pos(const char *s, size_t len, size_t max_cells, size_t *resume_at)
{
    /* Forward walk counting cells. A space sitting exactly at column
     * max_cells is a valid break point — its width "belongs" to the
     * fence between rows, not the row content, so we record it even
     * when adding its width would push us over. */
    size_t i = 0;
    size_t cells = 0;
    size_t last_space_byte = (size_t)-1;
    while (i < len) {
        size_t consumed;
        size_t w = cells_at(s, len, i, &consumed);
        if (cells + w > max_cells) {
            if (s[i] == ' ' && cells == max_cells)
                last_space_byte = i;
            break;
        }
        if (s[i] == ' ')
            last_space_byte = i;
        cells += w;
        i += consumed;
    }
    if (i >= len) {
        if (resume_at)
            *resume_at = len;
        return len;
    }
    if (last_space_byte == (size_t)-1) {
        size_t cut = advance_cells(s, len, max_cells);
        if (resume_at)
            *resume_at = cut;
        return cut;
    }
    /* Trim any preceding spaces from the row tail (defensive against
     * unflattened input — flatten_for_display normally collapses
     * multi-space runs, but the helper shouldn't depend on that). */
    size_t end = last_space_byte;
    while (end > 0 && s[end - 1] == ' ')
        end--;
    if (resume_at)
        *resume_at = last_space_byte + 1;
    return end;
}

size_t wrap_break_pos(const char *s, size_t len, size_t max_cells, size_t *resume_at)
{
    /* Precondition: max_cells >= 1 (see util.h). */
    assert(max_cells >= 1);
    size_t resume = 0;
    size_t end = strict_break_pos(s, len, max_cells, &resume);
    /* Forward-progress fallback: if the very first codepoint is
     * already wider than the budget (e.g. an emoji on a 1-cell row),
     * strict_break_pos returns 0 and the caller's outer wrap loop
     * would stall. Advance by one full codepoint instead — the
     * visible row overflows by ≤1 cell in that pathological case.
     * Truncate callers don't go through here. */
    if (end == 0 && resume == 0 && len > 0) {
        end = skip_zero_width(s, len, utf8_next(s, len, 0));
        resume = end;
    }
    if (resume_at)
        *resume_at = resume;
    return end;
}

char *reflow_for_display(const char *s, int first_row, int mid_row, int max_rows,
                         int last_row_reserve)
{
    if (!s)
        return xstrdup("");
    if (max_rows < 1)
        max_rows = 1;
    if (first_row < 1)
        first_row = 1;
    if (mid_row < 1)
        mid_row = 1;
    if (last_row_reserve < 0)
        last_row_reserve = 0;

    size_t len = strlen(s);
    int single_budget = first_row - last_row_reserve;
    if (single_budget < 1)
        single_budget = 1;
    /* Cheap fast-path for ASCII / short inputs: byte length <= budget
     * implies cell count <= budget (each cell takes >= 1 byte). For
     * non-ASCII we fall through and let the loop measure properly. */
    if (len <= (size_t)single_budget)
        return xstrdup(s);

    struct buf out;
    buf_init(&out);
    size_t pos = 0;
    for (int row = 0; row < max_rows; row++) {
        int width = (row == 0) ? first_row : mid_row;
        /* `last_width` is what the row gets if IT turns out to be the
         * final one — full width minus the caller's reserve so an
         * appended suffix doesn't push the row past `width`. Applies
         * both to natural termination (tail fits on this row) and
         * forced termination (row == max_rows - 1). Non-final rows
         * are free to use the full width since later rows absorb the
         * remainder. */
        int last_width = width - last_row_reserve;
        if (last_width < 1)
            last_width = 1;
        int forced_last = (row == max_rows - 1);
        size_t rem = len - pos;
        /* Probe with last_width: if the rest fits there, this row is
         * the final one and the suffix will fit too. */
        if (advance_cells(s + pos, rem, (size_t)last_width) == rem) {
            buf_append(&out, s + pos, rem);
            break;
        }
        if (forced_last) {
            /* Out of rows but content remains. Reserve 3 cells for
             * "..."; if the budget is so tight that "..." doesn't fit,
             * hard-cut at the row boundary as a last resort. */
            int target = last_width - 3;
            if (target < 1) {
                size_t cut = advance_cells(s + pos, rem, (size_t)last_width);
                buf_append(&out, s + pos, cut);
                break;
            }
            /* Use strict_break_pos (no forward-progress fallback) so a
             * wide first codepoint can't overshoot — we'd append "..."
             * after, and that would push the row past `width` cells. */
            size_t end = strict_break_pos(s + pos, rem, (size_t)target, NULL);
            buf_append(&out, s + pos, end);
            buf_append(&out, "...", 3);
            break;
        }
        /* Wrap with last_width (not full width). Even non-final rows
         * use the reduced budget so whichever row turns out to be
         * final has room for the caller's suffix — avoids the "tail
         * fits in width but not last_width" wart that would otherwise
         * leave a trailing \n and push the suffix to its own row. */
        size_t resume;
        size_t end = wrap_break_pos(s + pos, rem, (size_t)last_width, &resume);
        buf_append(&out, s + pos, end);
        buf_append(&out, "\n", 1);
        pos += resume;
    }
    /* Loop is guaranteed to append at least once (max_rows >= 1, and
     * each iteration ends in a buf_append before break/continue), so
     * out.data is non-NULL here in practice. The fallback keeps the
     * "never returns NULL" contract honest under NDEBUG / future
     * refactors. */
    if (!out.data)
        return xstrdup("");
    return buf_steal(&out);
}

int write_all(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int ensure_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode)) {
        errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        return -1;
    }
    return 0;
}

char *slurp_file(const char *path, size_t *out_len)
{
    if (ensure_regular_file(path) < 0)
        return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0)
        goto err_close;

    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf)
        goto err_close;

    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            goto err_free;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    buf[got] = '\0';
    close(fd);
    if (out_len)
        *out_len = got;
    return buf;

err_free:
    free(buf);
err_close:
    close(fd);
    return NULL;
}

char *slurp_file_capped(const char *path, size_t cap, size_t *out_len, int *out_truncated)
{
    if (ensure_regular_file(path) < 0)
        return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    char *buf = malloc(cap + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t got = 0;
    while (got < cap) {
        ssize_t r = read(fd, buf + got, cap - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }

    int truncated = 0;
    if (got == cap) {
        char probe;
        ssize_t r = read(fd, &probe, 1);
        if (r > 0)
            truncated = 1;
    }

    close(fd);
    buf[got] = '\0';
    if (out_len)
        *out_len = got;
    if (out_truncated)
        *out_truncated = truncated;
    return buf;
}

char *cap_line_lengths(const char *data, size_t len, size_t max_line, size_t *out_len)
{
    struct buf out;
    buf_init(&out);
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && data[i] != '\n')
            i++;
        size_t line_len = i - line_start;
        if (line_len > max_line) {
            buf_append(&out, data + line_start, max_line);
            char marker[64];
            int m = snprintf(marker, sizeof(marker), "...[%zu bytes elided]", line_len - max_line);
            buf_append(&out, marker, (size_t)m);
        } else {
            buf_append(&out, data + line_start, line_len);
        }
        if (i < len) {
            buf_append(&out, "\n", 1);
            i++;
        }
    }
    if (!out.data)
        out.data = xstrdup("");
    if (out_len)
        *out_len = out.len;
    return buf_steal(&out);
}

/* Cap on consecutive zero-width codepoints (combining marks,
 * variation selectors) attached to one base glyph. Devanagari,
 * Arabic, and other scripts use a handful per base; Unicode's Stream-
 * Safe Format upper bound is 30. Beyond this cap, additional zero-
 * width codepoints are model-supplied flooding (~1 visual cell for
 * arbitrary bytes — would let a hostile tool arg dump huge terminal
 * output and visually modify the preceding glyph). */
#define MAX_ZW_PER_BASE 8

char *flatten_for_display(const char *s)
{
    if (!s)
        return xstrdup("");
    size_t n = strlen(s);
    /* Output is at most n bytes — every transformation either drops
     * bytes (whitespace collapse, zero-width cap) or substitutes 1
     * byte ('?') for a multi-byte dangerous codepoint. */
    char *out = xmalloc(n + 1);
    size_t j = 0;
    int prev_space = 1; /* drop leading whitespace */
    int zw_run = 0;     /* consecutive zero-width codepoints emitted */
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            /* ASCII fast path: original whitespace-collapse + drop-
             * controls behavior. Locale-independent. */
            int is_space = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c < 0x20 || c == 0x7f;
            if (is_space) {
                if (!prev_space) {
                    out[j++] = ' ';
                    prev_space = 1;
                }
            } else {
                out[j++] = (char)c;
                prev_space = 0;
            }
            zw_run = 0;
            i++;
            continue;
        }
        /* Multi-byte UTF-8: utf8_codepoint_cells flags "dangerous"
         * codepoints (Trojan Source bidi vectors, ZWJ, BOM, malformed
         * sequences) by returning < 0, and zero-width legitimate
         * combining marks by returning 0. Substitute one '?' per
         * dangerous codepoint; cap consecutive zero-width runs at
         * MAX_ZW_PER_BASE; otherwise pass through. The width math
         * elsewhere substitutes 1 cell for w<0 and 0 cells for w==0,
         * so cell budgeting stays consistent with what we emit. */
        size_t consumed;
        int w = utf8_codepoint_cells(s, n, i, &consumed);
        if (w < 0) {
            out[j++] = '?';
            zw_run = 0;
            prev_space = 0;
        } else if (w == 0) {
            if (zw_run < MAX_ZW_PER_BASE) {
                for (size_t k = 0; k < consumed; k++)
                    out[j++] = s[i + k];
                zw_run++;
            }
            /* prev_space unchanged: zero-width doesn't change the
             * trailing-whitespace question. */
        } else {
            for (size_t k = 0; k < consumed; k++)
                out[j++] = s[i + k];
            zw_run = 0;
            prev_space = 0;
        }
        i += consumed ? consumed : 1;
    }
    while (j > 0 && out[j - 1] == ' ')
        j--;
    out[j] = '\0';
    return out;
}

char *expand_home(const char *path)
{
    if (!path)
        return NULL;
    if (path[0] != '~')
        return xstrdup(path);
    const char *home = getenv("HOME");
    if (!home)
        return xstrdup(path);
    return xasprintf("%s%s", home, path + 1);
}

/* Shared resolver behind xdg_hax_config_path / xdg_hax_state_path:
 * "$<env_var>/hax/<relpath>" when the env var is set and non-empty,
 * else "$HOME/<home_default>/hax/<relpath>". Returns NULL when neither
 * is available. */
static char *xdg_hax_path(const char *env_var, const char *home_default, const char *relpath)
{
    const char *xdg = getenv(env_var);
    if (xdg && *xdg)
        return xasprintf("%s/hax/%s", xdg, relpath);
    const char *home = getenv("HOME");
    if (home && *home)
        return xasprintf("%s/%s/hax/%s", home, home_default, relpath);
    return NULL;
}

char *xdg_hax_config_path(const char *relpath)
{
    return xdg_hax_path("XDG_CONFIG_HOME", ".config", relpath);
}

char *xdg_hax_state_path(const char *relpath)
{
    return xdg_hax_path("XDG_STATE_HOME", ".local/state", relpath);
}

char *dup_trim_trailing_slash(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/')
        n--;
    char *out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

long parse_duration_ms(const char *s)
{
    if (!s || !*s)
        return -1;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    /* ERANGE catches out-of-range numerals like "99...99ms" — strtol
     * clamps to LONG_MAX, and the mul==1 ms path below would otherwise
     * skip the overflow guard and silently accept a near-LONG_MAX value. */
    if (end == s || v < 0 || errno == ERANGE)
        return -1;
    while (*end == ' ' || *end == '\t')
        end++;
    long mul;
    /* `ms` must be matched before bare `m` so "5ms" doesn't parse as
     * "5m" (300_000) followed by stray "s". */
    if ((end[0] == 'm' || end[0] == 'M') && (end[1] == 's' || end[1] == 'S')) {
        mul = 1;
        end += 2;
    } else if (*end == '\0' || *end == 's' || *end == 'S') {
        mul = 1000;
        if (*end)
            end++;
    } else if (*end == 'm' || *end == 'M') {
        mul = 60000;
        end++;
    } else if (*end == 'h' || *end == 'H') {
        mul = 3600000;
        end++;
    } else {
        return -1;
    }
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return -1;
    /* Reject values that would overflow when scaled. v is non-negative;
     * only the multiply needs guarding. */
    if (mul > 1 && v > LONG_MAX / mul)
        return -1;
    return v * mul;
}

long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void buf_init(struct buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(struct buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void buf_grow(struct buf *b, size_t need)
{
    if (b->cap >= need)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(struct buf *b, const void *data, size_t n)
{
    buf_grow(b, b->len + n + 1);
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(struct buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_reset(struct buf *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

char *buf_steal(struct buf *b)
{
    char *p = b->data;
    buf_init(b);
    return p;
}

void item_free(struct item *it)
{
    if (!it)
        return;
    free(it->text);
    free(it->call_id);
    free(it->tool_name);
    free(it->tool_arguments_json);
    free(it->output);
    free(it->reasoning_json);
    memset(it, 0, sizeof(*it));
}
