/* SPDX-License-Identifier: MIT */
#include "trace.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

static FILE *trace_fp;
static int trace_init_done;

static void trace_close_atexit(void)
{
    if (trace_fp) {
        fclose(trace_fp);
        trace_fp = NULL;
    }
}

static FILE *get_fp(void)
{
    if (trace_init_done)
        return trace_fp;
    trace_init_done = 1;
    const char *path = getenv("HAX_TRACE");
    if (!path || !*path)
        return NULL;
    trace_fp = fopen(path, "ae");
    if (!trace_fp) {
        fprintf(stderr, "hax: HAX_TRACE: cannot open '%s' for append\n", path);
        return NULL;
    }
    setvbuf(trace_fp, NULL, _IOLBF, 0);
    atexit(trace_close_atexit);
    return trace_fp;
}

int trace_enabled(void)
{
    return get_fp() != NULL;
}

/* CommonMark: a fenced block must use a backtick run strictly longer than any
 * run inside its content. Default to 3, escalate if needed. */
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

/* Pretty-print a JSON payload inside a ```json fence. Falls back to ```text
 * with the raw bytes when parsing fails — keeps the trace useful even when
 * the server hands us garbage. */
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

static int header_name_is(const char *header, const char *name)
{
    size_t n = strlen(name);
    return strncasecmp(header, name, n) == 0 && header[n] == ':';
}

void trace_request(const char *url, const char *const *headers, const char *body, size_t body_len)
{
    FILE *fp = get_fp();
    if (!fp)
        return;

    fprintf(fp, "\n## POST %s\n\n", url);

    struct buf hb;
    buf_init(&hb);
    for (const char *const *h = headers; h && *h; h++) {
        if (header_name_is(*h, "Authorization")) {
            const char *colon = strchr(*h, ':');
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
        emit_json_or_text(fp, body, body_len);
}

void trace_response_status(long status, const char *error_body)
{
    FILE *fp = get_fp();
    if (!fp)
        return;

    fprintf(fp, "\n**HTTP %ld**\n", status);
    if (error_body && *error_body) {
        fputc('\n', fp);
        emit_json_or_text(fp, error_body, strlen(error_body));
    }
}

void trace_sse_event(const char *event_name, const char *data)
{
    FILE *fp = get_fp();
    if (!fp)
        return;

    const char *name = (event_name && *event_name) ? event_name : "(unnamed)";
    fprintf(fp, "\n### event: %s\n\n", name);
    if (data && *data)
        emit_json_or_text(fp, data, strlen(data));
}
