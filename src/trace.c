/* SPDX-License-Identifier: MIT */
#include "trace.h"

#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "util.h"

static pthread_mutex_t trace_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace_fp;
static int trace_init_done;

/* Wall-clock-since-first-entry plus delta-from-prior-entry, both in ms. Lets readers spot
 * pauses (a model going quiet between SSE chunks) without inferring from per-provider
 * `created` fields that may or may not update per chunk. Anchored on the first trace_* call
 * rather than process start so a `cat` of the file always starts at +0.000s. */
static int trace_have_start;
static long trace_start_ms;
static long trace_last_ms;

/* Caller must hold trace_mu. Writes "t+S.MMMs dt+S.MMMs" into out. */
static void format_ts_locked(char *out, size_t cap)
{
    long now = monotonic_ms();
    if (!trace_have_start) {
        trace_have_start = 1;
        trace_start_ms = now;
        trace_last_ms = now;
    }
    long abs_ms = now - trace_start_ms;
    long delta_ms = now - trace_last_ms;
    trace_last_ms = now;
    snprintf(out, cap, "t+%ld.%03lds dt+%ld.%03lds", abs_ms / 1000, abs_ms % 1000, delta_ms / 1000,
             delta_ms % 1000);
}

static void trace_close_atexit(void)
{
    pthread_mutex_lock(&trace_mu);
    if (trace_fp) {
        fclose(trace_fp);
        trace_fp = NULL;
    }
    pthread_mutex_unlock(&trace_mu);
}

static FILE *get_fp_locked(void)
{
    return trace_fp;
}

void trace_init(void)
{
    pthread_mutex_lock(&trace_mu);
    if (trace_init_done)
        goto out_unlock;
    trace_init_done = 1;
    const char *path = config_str("trace");
    if (!path || !*path)
        goto out_unlock;
    trace_fp = fopen(path, "we");
    if (!trace_fp) {
        hax_warn("HAX_TRACE: cannot open '%s' for writing", path);
        goto out_unlock;
    }
#ifdef _WIN32
    setvbuf(trace_fp, NULL, _IONBF, 0);
#else
    setvbuf(trace_fp, NULL, _IOLBF, 0);
#endif
    atexit(trace_close_atexit);

out_unlock:
    pthread_mutex_unlock(&trace_mu);
}

int trace_enabled(void)
{
    pthread_mutex_lock(&trace_mu);
    int enabled = get_fp_locked() != NULL;
    pthread_mutex_unlock(&trace_mu);
    return enabled;
}

/* CommonMark: a fenced block must use a backtick run strictly longer than any run inside its
 * content. Default to 3, escalate if needed. */
static size_t fence_len_for(const char *s, size_t n)
{
    size_t max_run = 0, cur = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '`') {
            cur++;
            if (cur > max_run)
                max_run = cur;
        } else {
            cur = 0;
        }
    }
    return max_run + 1 < 3 ? 3 : max_run + 1;
}

static void put_backticks(FILE *fp, size_t n)
{
    for (size_t i = 0; i < n; i++)
        fputc('`', fp);
}

static void emit_fenced(FILE *fp, const char *lang, const char *content, size_t len)
{
    size_t fence = fence_len_for(content, len);
    put_backticks(fp, fence);
    fprintf(fp, "%s\n", lang);
    if (len)
        fwrite(content, 1, len, fp);
    if (!len || content[len - 1] != '\n')
        fputc('\n', fp);
    put_backticks(fp, fence);
    fputc('\n', fp);
}

/* Falls back to a raw ```text fence when JSON parsing fails, so the trace stays useful when
 * the server sends garbage. */
static void emit_json_or_text(FILE *fp, const char *json, size_t len)
{
    json_error_t err;
    json_t *root = json_loadb(json, len, 0, &err);
    if (!root) {
        emit_fenced(fp, "text", json, len);
        return;
    }
    char *pretty = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    if (pretty) {
        emit_fenced(fp, "json", pretty, strlen(pretty));
        free(pretty);
    }
    json_decref(root);
}

/* Registered credential values, guarded by trace_mu; deliberately never freed. */
static char **trace_secrets;
static size_t trace_n_secrets;
static size_t trace_secrets_capacity;

void trace_register_secret(const char *value)
{
    if (!value || !*value)
        return;
    pthread_mutex_lock(&trace_mu);
    for (size_t i = 0; i < trace_n_secrets; i++) {
        if (strcmp(trace_secrets[i], value) == 0)
            goto out_unlock;
    }
    if (trace_n_secrets == trace_secrets_capacity) {
        trace_secrets_capacity = trace_secrets_capacity ? trace_secrets_capacity * 2 : 8;
        trace_secrets = xrealloc(trace_secrets, trace_secrets_capacity * sizeof(*trace_secrets));
    }
    trace_secrets[trace_n_secrets++] = xstrdup(value);
out_unlock:
    pthread_mutex_unlock(&trace_mu);
}

static const void *find_bytes(const char *haystack, size_t haystack_len, const char *needle,
                              size_t needle_len)
{
    if (needle_len == 0 || haystack_len < needle_len)
        return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (haystack[i] == needle[0] && memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

/* Caller must hold trace_mu. Returns an allocated copy of `text` with every registered credential
 * value replaced, or NULL when none occurs. OAuth-style bodies carry credentials as JSON values,
 * where header-line redaction cannot see them. */
static char *redact_secrets_locked(const char *text, size_t len, size_t *out_len)
{
    struct buf redacted;
    buf_init(&redacted);
    size_t position = 0;
    while (position < len) {
        const char *match = NULL;
        size_t match_len = 0;
        for (size_t i = 0; i < trace_n_secrets; i++) {
            size_t secret_len = strlen(trace_secrets[i]);
            const char *found =
                find_bytes(text + position, len - position, trace_secrets[i], secret_len);
            /* At equal positions the longest secret wins, or a registered prefix of another
             * secret would leave the tail of the longer one exposed. */
            if (found && (!match || found < match || (found == match && secret_len > match_len))) {
                match = found;
                match_len = secret_len;
            }
        }
        if (!match)
            break;
        buf_append(&redacted, text + position, (size_t)(match - (text + position)));
        buf_append_str(&redacted, "<redacted>");
        position = (size_t)(match - text) + match_len;
    }
    if (!redacted.data)
        return NULL;
    buf_append(&redacted, text + position, len - position);
    *out_len = redacted.len;
    return buf_steal(&redacted);
}

/* Caller must hold trace_mu. */
static void emit_body_redacted(FILE *fp, const char *body, size_t len)
{
    size_t redacted_len = 0;
    char *redacted = redact_secrets_locked(body, len, &redacted_len);
    emit_json_or_text(fp, redacted ? redacted : body, redacted ? redacted_len : len);
    free(redacted);
}

static int header_name_is(const char *header, const char *name)
{
    size_t n = strlen(name);
    return strncasecmp(header, name, n) == 0 && header[n] == ':';
}

/* Header names hax itself fills with credentials (api-key is Azure's spelling). */
static const char *const SENSITIVE_HEADERS[] = {"Authorization", "x-api-key", "api-key"};

/* Caller must hold trace_mu. A registered credential matches anywhere in the line, since it
 * may sit under any header name or inside a larger value such as "Bearer <token>". */
static int header_is_sensitive_locked(const char *header)
{
    for (size_t i = 0; i < sizeof(SENSITIVE_HEADERS) / sizeof(*SENSITIVE_HEADERS); i++) {
        if (header_name_is(header, SENSITIVE_HEADERS[i]))
            return 1;
    }
    for (size_t i = 0; i < trace_n_secrets; i++) {
        if (strstr(header, trace_secrets[i]))
            return 1;
    }
    return 0;
}

void trace_request(const char *method, const char *url, const char *const *headers,
                   const char *body, size_t body_len)
{
    pthread_mutex_lock(&trace_mu);
    FILE *fp = get_fp_locked();
    if (!fp)
        goto out_unlock;

    char ts[64];
    format_ts_locked(ts, sizeof(ts));
    fprintf(fp, "\n## %s %s  (%s)\n\n", method, url, ts);

    struct buf hb;
    buf_init(&hb);
    for (const char *const *h = headers; h && *h; h++) {
        const char *colon = strchr(*h, ':');
        if (colon && header_is_sensitive_locked(*h)) {
            buf_append(&hb, *h, colon - *h);
            buf_append_str(&hb, ": <redacted>\n");
        } else {
            buf_append_str(&hb, *h);
            buf_append(&hb, "\n", 1);
        }
    }
    if (hb.len)
        emit_fenced(fp, "http", hb.data, hb.len);
    buf_free(&hb);

    if (body && body_len)
        emit_body_redacted(fp, body, body_len);

out_unlock:
    pthread_mutex_unlock(&trace_mu);
}

void trace_response_status(long status, const char *error_body)
{
    pthread_mutex_lock(&trace_mu);
    FILE *fp = get_fp_locked();
    if (!fp)
        goto out_unlock;

    char ts[64];
    format_ts_locked(ts, sizeof(ts));
    fprintf(fp, "\n**HTTP %ld**  (%s)\n", status, ts);
    if (error_body && *error_body) {
        fputc('\n', fp);
        emit_body_redacted(fp, error_body, strlen(error_body));
    }

out_unlock:
    pthread_mutex_unlock(&trace_mu);
}

void trace_sse_event(const char *event_name, const char *data)
{
    pthread_mutex_lock(&trace_mu);
    FILE *fp = get_fp_locked();
    if (!fp)
        goto out_unlock;

    const char *name = (event_name && *event_name) ? event_name : "(unnamed)";
    char ts[64];
    format_ts_locked(ts, sizeof(ts));
    fprintf(fp, "\n### event: %s  (%s)\n\n", name, ts);
    if (data && *data)
        emit_json_or_text(fp, data, strlen(data));

out_unlock:
    pthread_mutex_unlock(&trace_mu);
}
