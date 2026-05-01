/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define READ_CAP     (256 * 1024)
#define MAX_LINE_LEN 2000

struct read_result {
    char *body;      /* malloc'd; "" on success-with-no-content */
    size_t body_len; /* byte length of body */
    int truncated;   /* result hit the byte cap before EOF/limit */
    int past_eof;    /* requested offset exceeded what's in the file */
    int is_binary;   /* NUL byte found in first chunk; refuse */
    long lines_seen; /* total lines streamed (only meaningful when past_eof) */
};

/* Append [chunk+run_start, chunk+end) to `out`, respecting `cap`. Returns
 * 1 if the cap was hit (some or all of the run was rejected) so the
 * caller can stop reading; 0 if the whole run fit. Empty runs are no-ops. */
static int append_capped(struct buf *out, const char *chunk, size_t run_start, size_t end,
                         size_t cap)
{
    if (end <= run_start)
        return 0;
    if (out->len >= cap)
        return 1;
    size_t run_len = end - run_start;
    if (out->len + run_len > cap) {
        buf_append(out, chunk + run_start, cap - out->len);
        return 1;
    }
    buf_append(out, chunk + run_start, run_len);
    return 0;
}

/* Stream the file in 8K chunks, scanning for newlines manually so we can
 * skip past the requested offset and accumulate up to `cap` bytes of body.
 * Memory is hard-bounded by `cap`: we never call into a primitive (like
 * getline) that allocates one line worth of buffer up front, so a
 * pathological 10GB single-line file is safe to read. /dev/zero and
 * other non-regular files are filtered out by the caller before we get
 * here, since open(O_RDONLY) on a FIFO without a writer would block.
 *
 * Bytes are appended in runs (between newlines or chunk boundaries)
 * rather than one at a time, so a 256K result takes ~32 buf_append calls
 * instead of 256K.
 *
 * Truncation flag fires only when we refuse content we've already read
 * — that's proof more bytes exist. A file whose size lands exactly at
 * the cap exits via read()==0 with the flag clear.
 *
 * Returns 0 on success (caller frees r->body), -1 on hard read error
 * (errno set). */
static int read_lines_capped(const char *path, long offset, long limit, size_t cap,
                             struct read_result *r)
{
    r->body = NULL;
    r->body_len = 0;
    r->truncated = 0;
    r->past_eof = 0;
    r->is_binary = 0;
    r->lines_seen = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct buf out;
    buf_init(&out);
    char chunk[8192];
    long lines_complete = 0;
    int collecting = (offset == 1);
    long taken = 0;
    int hit_eof = 0;
    int hit_cap = 0;
    int saw_data_in_current_line = 0;
    int first_chunk = 1;

    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            buf_free(&out);
            errno = saved;
            return -1;
        }
        if (n == 0) {
            hit_eof = 1;
            break;
        }

        /* Binary detection on the first chunk only — typical binaries
         * (executables, archives, images) have NUL bytes in their
         * headers, so an early sniff is reliable and bails before the
         * buffer grows. Source files with embedded NULs are rare; the
         * model can fall back to bash for those. */
        if (first_chunk) {
            for (ssize_t i = 0; i < n; i++) {
                if (chunk[i] == 0) {
                    r->is_binary = 1;
                    close(fd);
                    buf_free(&out);
                    return 0;
                }
            }
            first_chunk = 0;
        }

        size_t run_start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            char c = chunk[i];
            /* Track "any byte since last newline" in either mode so a
             * trailing line without a final newline can be counted on
             * EOF. Without this, a 3-byte file "abc" with offset=2
             * would report "file has 0 lines" instead of 1. */
            if (c != '\n')
                saw_data_in_current_line = 1;

            if (!collecting) {
                if (c == '\n') {
                    lines_complete++;
                    saw_data_in_current_line = 0;
                    if (lines_complete + 1 >= offset) {
                        collecting = 1;
                        run_start = i + 1;
                    }
                }
                continue;
            }

            if (c == '\n') {
                if (append_capped(&out, chunk, run_start, i + 1, cap)) {
                    hit_cap = 1;
                    goto end_loop;
                }
                run_start = i + 1;
                lines_complete++;
                taken++;
                saw_data_in_current_line = 0;
                if (limit > 0 && taken >= limit)
                    goto end_loop;
            }
        }
        /* End of chunk: flush whatever's been collected since the last
         * newline. The cap check inside append_capped handles overflow. */
        if (collecting && append_capped(&out, chunk, run_start, (size_t)n, cap)) {
            hit_cap = 1;
            goto end_loop;
        }
    }
end_loop:
    close(fd);

    /* Trailing line without a final newline still counts. `wc -l` would
     * ignore it, but the model would be confused if it disappeared from
     * the line count. taken only ticks when we were collecting — past-EOF
     * reporting cares about lines_complete. */
    if (saw_data_in_current_line && hit_eof) {
        lines_complete++;
        if (collecting)
            taken++;
    }

    r->lines_seen = lines_complete;
    r->truncated = hit_cap;

    /* Past-EOF: hit EOF without emitting any of the requested lines.
     * offset=1 on an empty file is a benign "no lines", not an error. */
    if (hit_eof && taken == 0 && (lines_complete > 0 || offset > 1)) {
        r->past_eof = 1;
        buf_free(&out);
        return 0;
    }

    if (!out.data) {
        r->body = xstrdup("");
        return 0;
    }
    r->body_len = out.len;
    r->body = buf_steal(&out);
    return 0;
}

static char *run(const char *args_json)
{
    json_error_t jerr;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &jerr);
    if (!root)
        return xasprintf("invalid arguments: %s", jerr.text);

    const char *path = json_string_value(json_object_get(root, "path"));
    if (!path || !*path) {
        json_decref(root);
        return xstrdup("missing 'path' argument");
    }

    long offset = 1;
    long limit = 0;
    json_t *jo = json_object_get(root, "offset");
    if (jo) {
        if (!json_is_integer(jo)) {
            json_decref(root);
            return xstrdup("'offset' must be an integer");
        }
        offset = (long)json_integer_value(jo);
        if (offset < 1) {
            json_decref(root);
            return xstrdup("'offset' must be >= 1");
        }
    }
    json_t *jl = json_object_get(root, "limit");
    if (jl) {
        if (!json_is_integer(jl)) {
            json_decref(root);
            return xstrdup("'limit' must be an integer");
        }
        limit = (long)json_integer_value(jl);
        if (limit < 1) {
            json_decref(root);
            return xstrdup("'limit' must be >= 1");
        }
    }

    /* Refuse non-regular files before opening: a FIFO with no writer
     * blocks on open(O_RDONLY) forever, /dev/zero would stream until
     * we hit the cap (correct memory-wise but useless to the model),
     * and reading a directory or socket is meaningless. The model
     * gets a clear error and can recover. */
    struct stat st;
    if (stat(path, &st) < 0) {
        char *msg = xasprintf("error reading %s: %s", path, strerror(errno));
        json_decref(root);
        return msg;
    }
    if (!S_ISREG(st.st_mode)) {
        char *msg = xasprintf("%s exists but is not a regular file", path);
        json_decref(root);
        return msg;
    }

    /* When no slice is requested, refuse oversize files upfront so the
     * model gets a useful error ("here's how big it is, narrow your
     * request") instead of a silently truncated prefix. With offset or
     * limit, streaming is the right thing — the model is asking for a
     * specific window and the cap may not even fire. */
    if (!jo && !jl && st.st_size > READ_CAP) {
        char *msg = xasprintf("%s is %lld bytes; cap is %d. Pass offset/limit to read a slice, "
                              "or use bash with grep/head/tail.",
                              path, (long long)st.st_size, READ_CAP);
        json_decref(root);
        return msg;
    }

    struct read_result rr;
    if (read_lines_capped(path, offset, limit, READ_CAP, &rr) < 0) {
        char *msg = xasprintf("error reading %s: %s", path, strerror(errno));
        json_decref(root);
        return msg;
    }

    if (rr.is_binary) {
        char *msg = xasprintf("%s appears to be binary (NUL byte found in first 8 KiB)", path);
        free(rr.body);
        json_decref(root);
        return msg;
    }
    json_decref(root);

    if (rr.past_eof) {
        char *msg = xasprintf("(file has %ld line%s; offset %ld is past EOF)", rr.lines_seen,
                              rr.lines_seen == 1 ? "" : "s", offset);
        free(rr.body);
        return msg;
    }

    /* Cap individual lines before UTF-8 sanitization so the marker text
     * is itself valid UTF-8 and unaffected. Minified JS, CSV with no
     * newlines, and similar can fill the byte cap with one line of
     * useless content; this turns each pathological line into a small
     * tagged stub instead. */
    size_t capped_len = 0;
    char *capped = cap_line_lengths(rr.body, rr.body_len, MAX_LINE_LEN, &capped_len);
    free(rr.body);

    char *clean = sanitize_utf8(capped, capped_len);
    free(capped);

    if (rr.truncated) {
        char *msg = xasprintf("%s\n\n[truncated at %d bytes; file is larger]", clean, READ_CAP);
        free(clean);
        return msg;
    }
    return clean;
}

/* Render a short ":N-M" suffix when the model asked for a line range, so
 * the user sees what slice was requested without needing to read the JSON
 * args. Open-ended (only offset, no limit) renders as ":N-". */
static char *format_display_extra(const char *args_json)
{
    if (!args_json)
        return NULL;
    json_error_t jerr;
    json_t *root = json_loads(args_json, 0, &jerr);
    if (!root)
        return NULL;
    json_t *jo = json_object_get(root, "offset");
    json_t *jl = json_object_get(root, "limit");
    char *out = NULL;
    if (jo || jl) {
        long offset = (jo && json_is_integer(jo)) ? (long)json_integer_value(jo) : 1;
        if (jl && json_is_integer(jl)) {
            long limit = (long)json_integer_value(jl);
            /* `offset + limit - 1` would invoke signed overflow (UB) for
             * adversarial inputs near LONG_MAX. Tool args come from the
             * model and the schema has no maximum, so guard before the
             * addition. Garbage limits (<= 0) fall back to the open-ended
             * ":N-" form; the tool itself will reject them when run. */
            if (limit < 1) {
                out = xasprintf(":%ld-", offset);
            } else {
                long end = (offset > LONG_MAX - limit + 1) ? LONG_MAX : offset + limit - 1;
                out = xasprintf(":%ld-%ld", offset, end);
            }
        } else {
            out = xasprintf(":%ld-", offset);
        }
    }
    json_decref(root);
    return out;
}

const struct tool TOOL_READ = {
    .def =
        {
            .name = "read",
            .description =
                "Read a file from disk and return its contents. Optional 1-indexed line "
                "`offset` and `limit` slice a range; without them, the whole file is returned.",
            .parameters_schema_json =
                "{\"type\":\"object\","
                "\"properties\":{"
                "\"path\":{\"type\":\"string\",\"description\":\"Path to the file.\"},"
                "\"offset\":{\"type\":\"integer\",\"minimum\":1,"
                "\"description\":\"1-indexed first line to return. Default 1.\"},"
                "\"limit\":{\"type\":\"integer\",\"minimum\":1,"
                "\"description\":\"Maximum number of lines to return. Default: to EOF.\"}"
                "},"
                "\"required\":[\"path\"]}",
            .display_arg = "path",
        },
    .run = run,
    .format_display_extra = format_display_extra,
};
