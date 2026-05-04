/* SPDX-License-Identifier: MIT */
#include "codex.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ansi.h"
#include "bg.h"
#include "codex_events.h"
#include "http.h"
#include "interrupt.h"
#include "probe.h"
#include "progress.h"
#include "spinner.h"
#include "util.h"

#define CODEX_ENDPOINT        "https://chatgpt.com/backend-api/codex/responses"
#define CODEX_USAGE_ENDPOINT  "https://chatgpt.com/backend-api/wham/usage"
#define CODEX_MODELS_ENDPOINT "https://chatgpt.com/backend-api/codex/models"

/* Generous enough for the catalog over a wonky link, short enough that a
 * dead probe doesn't block shutdown noticeably (the bg cancel hook
 * usually aborts well before this fires anyway). */
#define CODEX_PROBE_TIMEOUT_S 5

/* Sent as the `client_version` query parameter on /models. The backend
 * filters out models whose minimal_client_version is newer than this
 * value; use a deliberately high synthetic version so metadata for
 * already-sendable models (for example gpt-5.5) is visible. */
#define CODEX_PROBE_CLIENT_VERSION "999.0.0"

struct codex {
    struct provider base;
    char *access_token;
    char *account_id;
    char *email; /* extracted from the id_token JWT — informational only (shown in
                  * /usage). NULL when the auth.json had no id_token, or the JWT was
                  * unparseable, or it didn't carry an "email" claim. */
    char *default_model;
    char *default_reasoning_effort;
    char *session_id; /* sent as prompt_cache_key — stable for process lifetime
                                             so the server can hit its prefix cache across turns */
    /* Optional context-window probe spawned at construction; joined by
     * codex_destroy. Local rather than on struct provider so codex
     * stays free to grow more probes (capabilities, account state, ...)
     * without redesigning the generic interface. */
    struct bg_job *probe;
};

/* ---------- Codex config (~/.codex/config.toml) ---------- */

static const char *skip_inline_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

static int key_matches(const char *p, const char *end, const char *key)
{
    size_t n = strlen(key);
    if ((size_t)(end - p) < n || memcmp(p, key, n) != 0)
        return 0;
    p += n;
    return p == end || *p == '=' || *p == ' ' || *p == '\t';
}

static char *parse_toml_string(const char *p, const char *end)
{
    if (p >= end || (*p != '"' && *p != '\''))
        return NULL;

    char quote = *p++;
    struct buf b;
    buf_init(&b);

    while (p < end) {
        char c = *p++;
        if (c == quote)
            return buf_steal(&b);

        if (quote == '"' && c == '\\' && p < end) {
            c = *p++;
            switch (c) {
            case 'b':
                c = '\b';
                break;
            case 't':
                c = '\t';
                break;
            case 'n':
                c = '\n';
                break;
            case 'f':
                c = '\f';
                break;
            case 'r':
                c = '\r';
                break;
            case '"':
            case '\\':
                break;
            default:
                /* Model names / effort values are ordinary ASCII strings.
                 * For unsupported escapes, keep the escaped byte rather
                 * than rejecting the whole config. */
                break;
            }
        }
        buf_append(&b, &c, 1);
    }

    buf_free(&b);
    return NULL;
}

static char *parse_top_level_toml_string_key(const char *contents, size_t len, const char *key)
{
    const char *p = contents;
    const char *end = contents + len;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end)
            line_end = end;

        const char *q = skip_inline_ws(p, line_end);
        if (q < line_end && *q == '[')
            return NULL; /* subsequent assignments are inside a table */
        if (q < line_end && *q != '#' && key_matches(q, line_end, key)) {
            q += strlen(key);
            q = skip_inline_ws(q, line_end);
            if (q < line_end && *q == '=') {
                q++;
                q = skip_inline_ws(q, line_end);
                return parse_toml_string(q, line_end);
            }
        }

        p = line_end < end ? line_end + 1 : end;
    }

    return NULL;
}

static void load_codex_settings(char **out_model, char **out_reasoning_effort)
{
    *out_model = NULL;
    *out_reasoning_effort = NULL;

    char *path = expand_home("~/.codex/config.toml");
    size_t len = 0;
    char *contents = slurp_file(path, &len);
    free(path);
    if (!contents)
        return;

    *out_model = parse_top_level_toml_string_key(contents, len, "model");
    *out_reasoning_effort =
        parse_top_level_toml_string_key(contents, len, "model_reasoning_effort");
    free(contents);
}

/* ---------- JWT email extraction ---------- */

/* Decode one base64 sextet. Returns 0..63 or -1 for non-alphabet bytes.
 * Caller is expected to have already mapped base64url ('-'/'_') onto
 * standard base64 ('+'/'/') and replaced missing pad with '='. */
static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* Decode a base64url segment to a freshly-allocated NUL-terminated
 * string. NULL on malformed input. The caller treats the result as
 * UTF-8 text (it's the JWT JSON payload, which is always UTF-8). */
static char *b64url_decode(const char *seg, size_t n)
{
    char *norm = xmalloc(n + 4);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = seg[i];
        norm[k++] = (c == '-') ? '+' : (c == '_') ? '/' : c;
    }
    while (k % 4)
        norm[k++] = '=';

    unsigned char *out = xmalloc((k / 4) * 3 + 1);
    size_t out_len = 0;
    for (size_t i = 0; i < k; i += 4) {
        int v[4];
        for (int j = 0; j < 4; j++) {
            unsigned char c = (unsigned char)norm[i + j];
            if (c == '=') {
                v[j] = 0;
            } else {
                int x = b64_value(c);
                if (x < 0) {
                    free(norm);
                    free(out);
                    return NULL;
                }
                v[j] = x;
            }
        }
        out[out_len++] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
        if (norm[i + 2] != '=')
            out[out_len++] = (unsigned char)((v[1] << 4) | (v[2] >> 2));
        if (norm[i + 3] != '=')
            out[out_len++] = (unsigned char)((v[2] << 6) | v[3]);
    }
    free(norm);
    out[out_len] = '\0';
    return (char *)out;
}

/* Extract the "email" claim from a JWT id_token. Best-effort: returns
 * NULL on any failure (malformed JWT, undecodable payload, no email
 * claim). The token is not validated — we trust ~/.codex/auth.json,
 * which `codex login` writes with restrictive perms; we just want a
 * human-readable label for the /usage header.
 *
 * Falls back to the namespaced "https://api.openai.com/profile" claim
 * when the top-level "email" is absent — matches codex-rs's parser
 * (login/src/token_data.rs:parse_chatgpt_jwt_claims). Tokens issued
 * via certain auth flows only carry the namespaced claim. */
static char *jwt_extract_email(const char *jwt)
{
    if (!jwt || !*jwt)
        return NULL;
    const char *p1 = strchr(jwt, '.');
    if (!p1)
        return NULL;
    const char *p2 = strchr(p1 + 1, '.');
    if (!p2)
        return NULL;

    char *payload = b64url_decode(p1 + 1, (size_t)(p2 - (p1 + 1)));
    if (!payload)
        return NULL;

    json_error_t err;
    json_t *root = json_loads(payload, 0, &err);
    free(payload);
    if (!root)
        return NULL;

    const char *email = json_string_value(json_object_get(root, "email"));
    if (!email || !*email) {
        json_t *profile = json_object_get(root, "https://api.openai.com/profile");
        if (json_is_object(profile))
            email = json_string_value(json_object_get(profile, "email"));
    }
    char *ret = (email && *email) ? xstrdup(email) : NULL;
    json_decref(root);
    return ret;
}

/* ---------- request body construction ---------- */

static json_t *build_input_items(const struct item *items, size_t n)
{
    json_t *arr = json_array();
    for (size_t i = 0; i < n; i++) {
        const struct item *it = &items[i];
        json_t *obj = NULL;

        switch (it->kind) {
        case ITEM_USER_MESSAGE: {
            json_t *content = json_array();
            json_array_append_new(content, json_pack("{s:s, s:s}", "type", "input_text", "text",
                                                     it->text ? it->text : ""));
            obj =
                json_pack("{s:s, s:s, s:o}", "type", "message", "role", "user", "content", content);
            break;
        }
        case ITEM_ASSISTANT_MESSAGE: {
            json_t *content = json_array();
            json_array_append_new(content, json_pack("{s:s, s:s}", "type", "output_text", "text",
                                                     it->text ? it->text : ""));
            obj = json_pack("{s:s, s:s, s:o}", "type", "message", "role", "assistant", "content",
                            content);
            break;
        }
        case ITEM_TOOL_CALL:
            obj = json_pack("{s:s, s:s, s:s, s:s}", "type", "function_call", "call_id",
                            it->call_id ? it->call_id : "", "name",
                            it->tool_name ? it->tool_name : "", "arguments",
                            it->tool_arguments_json ? it->tool_arguments_json : "{}");
            break;
        case ITEM_TOOL_RESULT:
            obj = json_pack("{s:s, s:s, s:s}", "type", "function_call_output", "call_id",
                            it->call_id ? it->call_id : "", "output", it->output ? it->output : "");
            break;
        case ITEM_REASONING:
            /* The blob was already whitelisted to valid input fields when
             * we received it (see codex_events.c) — just parse and emit. */
            if (it->reasoning_json) {
                json_error_t jerr;
                obj = json_loads(it->reasoning_json, 0, &jerr);
            }
            break;
        case ITEM_TURN_BOUNDARY:
            /* Agent-side marker for the transcript renderer; nothing to
             * send over the wire. */
            break;
        }
        if (obj)
            json_array_append_new(arr, obj);
    }
    return arr;
}

static json_t *build_tools(const struct tool_def *tools, size_t n)
{
    json_t *arr = json_array();
    for (size_t i = 0; i < n; i++) {
        json_error_t err;
        json_t *params = json_loads(tools[i].parameters_schema_json, 0, &err);
        if (!params) {
            fprintf(stderr, "hax: bad tool schema for %s: %s\n", tools[i].name, err.text);
            params = json_object();
        }
        json_array_append_new(arr, json_pack("{s:s, s:s, s:s, s:o}", "type", "function", "name",
                                             tools[i].name, "description", tools[i].description,
                                             "parameters", params));
    }
    return arr;
}

static char *build_body(const struct context *ctx, const char *model, const char *cache_key)
{
    json_t *include = json_array();
    json_array_append_new(include, json_string("reasoning.encrypted_content"));

    json_t *body =
        json_pack("{s:s, s:b, s:b, s:s, s:o, s:o, s:{s:s}, s:s, s:b, s:o}", "model", model, "store",
                  0, "stream", 1, "instructions", ctx->system_prompt ? ctx->system_prompt : "",
                  "input", build_input_items(ctx->items, ctx->n_items), "include", include, "text",
                  "verbosity", "medium", "tool_choice", "auto", "parallel_tool_calls", 1, "tools",
                  build_tools(ctx->tools, ctx->n_tools));

    if (cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(cache_key));

    if (ctx->reasoning_effort)
        json_object_set_new(
            body, "reasoning",
            json_pack("{s:s, s:s}", "effort", ctx->reasoning_effort, "summary", "auto"));

    char *s = json_dumps(body, JSON_COMPACT);
    json_decref(body);
    return s;
}

/* ---------- SSE glue ---------- */

static int on_sse(const char *event_name, const char *data, void *user)
{
    (void)event_name; /* Codex mirrors the type in the data JSON */
    codex_events_feed(user, data);
    return 0;
}

/* ---------- provider interface ---------- */

static int codex_stream(struct provider *p, const struct context *ctx, const char *model,
                        stream_cb cb, void *user)
{
    struct codex *c = (struct codex *)p;

    char *body = build_body(ctx, model, c->session_id);
    if (!body)
        return -1;
    size_t body_len = strlen(body);

    char *auth_hdr = xasprintf("Authorization: Bearer %s", c->access_token);
    char *acct_hdr = xasprintf("chatgpt-account-id: %s", c->account_id);
    /* Reuse the per-process UUID we already mint for prompt_cache_key.
     * pi-mono sends both headers with the same value; codex-rs likewise.
     * Used by the server for routing/dedup affinity. */
    char *sess_hdr = xasprintf("session_id: %s", c->session_id);
    char *reqid_hdr = xasprintf("x-client-request-id: %s", c->session_id);
#if defined(__APPLE__)
    const char *ua = "User-Agent: hax/0.1 (macos)";
#elif defined(__linux__)
    const char *ua = "User-Agent: hax/0.1 (linux)";
#else
    const char *ua = "User-Agent: hax/0.1";
#endif

    const char *headers[] = {
        auth_hdr,
        acct_hdr,
        sess_hdr,
        reqid_hdr,
        "originator: hax",
        ua,
        "OpenAI-Beta: responses=experimental",
        "Accept: text/event-stream",
        "Content-Type: application/json",
        NULL,
    };

    struct codex_events ev;
    codex_events_init(&ev, cb, user);
    struct http_response resp;
    int rc = http_sse_post(CODEX_ENDPOINT, headers, body, body_len, on_sse, &ev,
                           interrupt_requested, &resp);

    if (resp.cancelled) {
        /* User-initiated abort — agent layer handles the partial state
         * and the "[interrupted]" notice. Don't surface as EV_ERROR. */
    } else if (resp.status == 401) {
        struct stream_event e = {
            .kind = EV_ERROR,
            .u.error = {.message = "codex token expired — run `codex` "
                                   "once to refresh, then retry",
                        .http_status = 401},
        };
        cb(&e, user);
    } else if (rc != 0 || resp.status < 200 || resp.status >= 300) {
        struct stream_event e = {
            .kind = EV_ERROR,
            .u.error = {.message = resp.error_body ? resp.error_body : "request failed",
                        .http_status = (int)resp.status},
        };
        cb(&e, user);
    } else {
        codex_events_finalize(&ev);
    }

    free(resp.error_body);
    free(auth_hdr);
    free(acct_hdr);
    free(sess_hdr);
    free(reqid_hdr);
    free(body);
    codex_events_free(&ev);
    return rc;
}

/* ---------- /models context-window probe ---------- */

/* Walk the chatgpt.com catalog response shape: `{ "models": [ { "slug":
 * ..., "context_window": N }, ... ] }` and pull the context_window for
 * the entry whose `slug` matches the model we're about to use. `user`
 * is the heap-owned model slug captured at spawn time. */
static long extract_codex_context(const char *body, void *user)
{
    const char *model = user;
    if (!model || !*model)
        return 0;
    json_t *root = json_loads(body, 0, NULL);
    if (!root)
        return 0;
    long out = 0;
    json_t *models = json_object_get(root, "models");
    if (json_is_array(models)) {
        size_t i;
        json_t *entry;
        json_array_foreach(models, i, entry)
        {
            json_t *slug = json_object_get(entry, "slug");
            if (!json_is_string(slug) || strcmp(json_string_value(slug), model) != 0)
                continue;
            json_t *ctx = json_object_get(entry, "context_window");
            if (!json_is_integer(ctx) || json_integer_value(ctx) <= 0)
                ctx = json_object_get(entry, "max_context_window");
            if (json_is_integer(ctx) && json_integer_value(ctx) > 0)
                out = (long)json_integer_value(ctx);
            break;
        }
    }
    json_decref(root);
    return out;
}

static void spawn_context_probe(struct codex *c)
{
    /* User-supplied HAX_CONTEXT_LIMIT wins; nothing for the probe to
     * add and we save a network round-trip + the matching shutdown
     * join cost. */
    const char *cur = getenv("HAX_CONTEXT_LIMIT");
    if (cur && *cur)
        return;
    /* Resolve the model slug we'll actually send. HAX_MODEL wins per
     * agent.c's resolver; otherwise the codex default applies. The
     * probe needs a slug to match against the catalog — without one
     * there's nothing useful to look up. */
    const char *env_model = getenv("HAX_MODEL");
    const char *model =
        (env_model && *env_model) ? env_model : (c->default_model ? c->default_model : "");
    if (!*model)
        return;

    struct probe_args *a = xcalloc(1, sizeof(*a));
    a->url = xasprintf("%s?client_version=%s", CODEX_MODELS_ENDPOINT, CODEX_PROBE_CLIENT_VERSION);
    /* Same auth headers we already build for /responses; the chatgpt
     * backend accepts the bearer token + account-id pair on /models
     * too. originator + UA mirror the streaming path so the probe
     * looks like the same client to server-side telemetry. */
    a->headers = xcalloc(5, sizeof(*a->headers));
    a->headers[0] = xasprintf("Authorization: Bearer %s", c->access_token);
    a->headers[1] = xasprintf("chatgpt-account-id: %s", c->account_id);
    a->headers[2] = xstrdup("originator: hax");
    a->headers[3] = xstrdup("Accept: application/json");
    a->headers[4] = NULL;
    a->timeout_s = CODEX_PROBE_TIMEOUT_S;
    a->extract = extract_codex_context;
    a->user = xstrdup(model);
    a->free_user = free;
    a->target = &c->base.context_limit;
    c->probe = probe_context_limit_spawn(a);
}

/* ---------- /usage ---------- */

/* Format the wall-clock time the window resets. If `reset_at` lands on
 * today (local time) we show only HH:MM since the date is implied;
 * anything further out picks up a short month-day prefix so a weekly
 * window's "resets Mon" is unambiguous. Output written into `out`,
 * NUL-terminated. */
static void format_reset_time(char *out, size_t cap, time_t reset_at)
{
    time_t now = time(NULL);
    struct tm reset_tm, now_tm;
    if (!localtime_r(&reset_at, &reset_tm) || !localtime_r(&now, &now_tm)) {
        snprintf(out, cap, "?");
        return;
    }
    int same_day = reset_tm.tm_year == now_tm.tm_year && reset_tm.tm_yday == now_tm.tm_yday;
    if (same_day)
        strftime(out, cap, "%H:%M", &reset_tm);
    else
        /* %d is zero-padded (ISO C). %-d / %e both produce nicer "5"
         * output — but %-d is a GNU extension that warns on stricter
         * compilers, and %e space-pads ("May  5") which reads awkward
         * in running text. "May 05" is the least-bad portable choice. */
        strftime(out, cap, "%a %b %d, %H:%M", &reset_tm);
}

/* Visual layout constants. LABEL_W matches the longest label we emit
 * ("weekly"); BAR_W is the bar's cell count. ROW_INDENT is the column
 * the bar starts at — also the indent used for the wrapped "resets"
 * row on narrow terminals so it lines up under the bar. */
#define USAGE_LABEL_W    6
#define USAGE_BAR_W      20
#define USAGE_ROW_INDENT (2 + USAGE_LABEL_W + 1) /* "  " + label + " " */

/* Derive a window's display label from its `limit_window_seconds`. The
 * Codex backend's two named slots (primary_window, secondary_window)
 * don't carry the human label themselves — the duration is what
 * distinguishes them, and the duration varies by plan. Common values
 * get a friendlier name ("weekly", "daily"); anything else falls back
 * to a generic Ns/Nm/Nh/Nd format so an unfamiliar bucket still
 * renders correctly. Output is bounded by USAGE_LABEL_W in the common
 * case; longer labels just push the row's bar slightly right, which
 * is preferable to a wrong-but-aligned hardcoded string. */
static void format_window_label(char *out, size_t cap, int secs)
{
    if (secs <= 0)
        snprintf(out, cap, "?");
    else if (secs == 604800)
        snprintf(out, cap, "weekly");
    else if (secs == 86400)
        snprintf(out, cap, "daily");
    else if (secs < 60)
        snprintf(out, cap, "%ds", secs);
    else if (secs < 3600)
        snprintf(out, cap, "%dm", secs / 60);
    else if (secs < 86400)
        snprintf(out, cap, "%dh", secs / 3600);
    else
        snprintf(out, cap, "%dd", secs / 86400);
}

/* Render one labeled bar:
 *
 *   5h     ████████░░░░░░░░░░░   42% used · resets 18:42
 *
 * On terminals too narrow to fit the full row, the "· resets X" tail
 * reflows onto its own line indented under the bar:
 *
 *   weekly ████████░░░░░░░░░░░   42% used
 *          resets Mon Dec 15, 18:42
 *
 * The whole row renders dim — /usage is a meta status block, intended
 * to recede visually rather than compete with conversation text. The
 * label is derived from the response's `limit_window_seconds`, so the
 * row reads correctly across plans with different bucket durations
 * (Pro/Team have 5h+weekly today; other plans may differ). `win` is
 * the JSON window snapshot or NULL — when absent (e.g. plan with only
 * one window) the row is skipped silently. `slot` is the API's
 * positional name ("primary" / "secondary"), used only as a fallback
 * label when the data-derived one isn't available.
 *
 * Field types are validated: codex-rs declares used_percent / reset_at
 * as i32, but if the schema drifts (used_percent → float, reset_at →
 * string, fields renamed) we'd otherwise render convincing garbage
 * like "0% used · resets Jan 01, 1970". An unrecognized shape gets a
 * visible marker line instead. used_percent is read as a number
 * (integer or real) for forward-compat. */
static void print_window(const char *slot, json_t *win)
{
    if (!win || json_is_null(win))
        return;

    json_t *jup = json_object_get(win, "used_percent");
    json_t *jra = json_object_get(win, "reset_at");
    json_t *jlws = json_object_get(win, "limit_window_seconds");
    if (!json_is_number(jup) || !json_is_number(jra)) {
        printf("  " ANSI_DIM "%-*s (unrecognized window shape)" ANSI_RESET "\n", USAGE_LABEL_W,
               slot);
        return;
    }

    char label[16];
    if (json_is_number(jlws))
        format_window_label(label, sizeof(label), (int)json_integer_value(jlws));
    else
        snprintf(label, sizeof(label), "%s", slot);

    double used = json_number_value(jup);
    if (used < 0)
        used = 0;
    if (used > 100)
        used = 100;
    time_t reset_at = (time_t)json_number_value(jra);

    char when[64];
    format_reset_time(when, sizeof(when), reset_at);

    char pct_seg[32];
    int pct_len = snprintf(pct_seg, sizeof(pct_seg), " %3d%% used", (int)(used + 0.5));
    char tail_seg[96];
    int tail_len = snprintf(tail_seg, sizeof(tail_seg), " · resets %s", when);

    /* Probe column count and decide single-line vs reflow. The bar
     * itself is BAR_W cells; the rest is plain ASCII so byte length
     * equals visible width. */
    int total = USAGE_ROW_INDENT + USAGE_BAR_W + pct_len + tail_len;
    int reflow = total > term_width();

    printf("  " ANSI_DIM "%-*s" ANSI_RESET " ", USAGE_LABEL_W, label);
    progress_bar_print(used / 100.0, USAGE_BAR_W);
    if (!reflow) {
        printf(ANSI_DIM "%s%s" ANSI_RESET "\n", pct_seg, tail_seg);
    } else {
        printf(ANSI_DIM "%s" ANSI_RESET "\n", pct_seg);
        printf("%*s" ANSI_DIM "resets %s" ANSI_RESET "\n", USAGE_ROW_INDENT, "", when);
    }
}

static int codex_query_usage(struct provider *p)
{
    struct codex *c = (struct codex *)p;

    char *auth_hdr = xasprintf("Authorization: Bearer %s", c->access_token);
    char *acct_hdr = xasprintf("chatgpt-account-id: %s", c->account_id);
    const char *headers[] = {
        auth_hdr, acct_hdr, "originator: hax", "Accept: application/json", NULL,
    };

    /* Single round-trip to chatgpt.com; usually <1s but can stretch on
     * a flaky link. The slash dispatcher already emitted the leading
     * blank-line gap, so the spinner draws in the row that the codex
     * header will eventually occupy and the layout stays stable from
     * the moment /usage is issued. Spinner is a no-op on non-TTY stdout
     * so this stays safe under `hax -p '/usage'`-style scripted
     * invocations. */
    struct spinner *sp = spinner_new("fetching usage...");
    spinner_show(sp);
    char *body = NULL;
    int rc = http_get(CODEX_USAGE_ENDPOINT, headers, 30, NULL, &body);
    spinner_hide(sp);
    spinner_free(sp);
    free(auth_hdr);
    free(acct_hdr);
    if (rc != 0 || !body) {
        fprintf(stderr, "hax: failed to fetch usage from %s\n", CODEX_USAGE_ENDPOINT);
        free(body);
        return -1;
    }

    json_error_t jerr;
    json_t *root = json_loads(body, 0, &jerr);
    free(body);
    if (!root) {
        fprintf(stderr, "hax: usage response is not valid JSON: %s\n", jerr.text);
        return -1;
    }

    const char *plan = json_string_value(json_object_get(root, "plan_type"));
    json_t *rl = json_object_get(root, "rate_limit");

    printf(ANSI_DIM "codex");
    if (c->email)
        printf(" · %s", c->email);
    if (plan && *plan)
        printf(" · %s", plan);
    printf(ANSI_RESET "\n");

    if (rl && !json_is_null(rl)) {
        print_window("primary", json_object_get(rl, "primary_window"));
        print_window("secondary", json_object_get(rl, "secondary_window"));
    } else {
        /* Plans where the active limiting bucket lives elsewhere in the
         * payload (the response also carries `credits` for usage-based
         * plans and `additional_rate_limits` for accounts with extra
         * workspace/member buckets) currently fall through here even
         * though there's quota data we could be rendering. Left as a
         * known limitation until we have a concrete payload from one
         * of those plans to design the layout against — guessing the
         * shape from the codex-rs serializer alone is more likely to
         * mis-render than to help. The honest "nothing rendered here"
         * message is preferred over inventing a UX. */
        printf("  " ANSI_DIM "no rate-limit windows reported for this plan" ANSI_RESET "\n");
    }

    json_decref(root);
    return 0;
}

static void codex_destroy(struct provider *p)
{
    struct codex *c = (struct codex *)p;
    /* Settle the bg probe before freeing what it writes to. Cancel
     * first so the worker exits its http_get promptly via the bg
     * cancel thunk wired through the progress callback. */
    if (c->probe) {
        bg_cancel(c->probe);
        bg_join(c->probe);
    }
    free(c->access_token);
    free(c->account_id);
    free(c->email);
    free(c->default_model);
    free(c->default_reasoning_effort);
    free(c->session_id);
    free(c);
}

struct provider *codex_provider_new(void)
{
    char *path = expand_home("~/.codex/auth.json");
    size_t len = 0;
    char *contents = slurp_file(path, &len);
    if (!contents) {
        fprintf(stderr, "hax: cannot read %s — is the codex CLI installed and logged in?\n", path);
        free(path);
        return NULL;
    }
    free(path);

    json_error_t err;
    json_t *root = json_loads(contents, 0, &err);
    free(contents);
    if (!root) {
        fprintf(stderr, "hax: ~/.codex/auth.json is not valid JSON: %s\n", err.text);
        return NULL;
    }

    json_t *tokens = json_object_get(root, "tokens");
    const char *access = tokens ? json_string_value(json_object_get(tokens, "access_token")) : NULL;
    const char *account = tokens ? json_string_value(json_object_get(tokens, "account_id")) : NULL;
    const char *id_token = tokens ? json_string_value(json_object_get(tokens, "id_token")) : NULL;
    if (!access || !account) {
        fprintf(stderr, "hax: auth.json missing tokens.access_token or tokens.account_id\n");
        json_decref(root);
        return NULL;
    }

    char *cfg_model = NULL;
    char *cfg_reasoning_effort = NULL;
    load_codex_settings(&cfg_model, &cfg_reasoning_effort);
    if (cfg_model && !*cfg_model) {
        free(cfg_model);
        cfg_model = NULL;
    }
    if (cfg_reasoning_effort && !*cfg_reasoning_effort) {
        free(cfg_reasoning_effort);
        cfg_reasoning_effort = NULL;
    }

    struct codex *c = xcalloc(1, sizeof(*c));
    c->default_model = cfg_model ? cfg_model : xstrdup("gpt-5.3-codex");
    c->default_reasoning_effort = cfg_reasoning_effort;
    c->base.name = "codex";
    c->base.default_model = c->default_model;
    c->base.default_reasoning_effort = c->default_reasoning_effort;
    c->base.stream = codex_stream;
    c->base.query_usage = codex_query_usage;
    c->base.destroy = codex_destroy;
    c->access_token = xstrdup(access);
    c->account_id = xstrdup(account);
    c->email = jwt_extract_email(id_token); /* may be NULL — fine */
    char uuid[37];
    gen_uuid_v4(uuid);
    c->session_id = xstrdup(uuid);

    json_decref(root);
    /* Best-effort context-window probe runs in the background — gives
     * the agent a "%" once the catalog response lands without delaying
     * the first prompt. Failure (expired token, network blip, missing
     * slug) silently leaves the percentage display hidden. */
    spawn_context_probe(c);
    return &c->base;
}

const struct provider_factory PROVIDER_CODEX = {
    .name = "codex",
    .new = codex_provider_new,
};
