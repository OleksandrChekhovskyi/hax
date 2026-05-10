/* SPDX-License-Identifier: MIT */
#include "mock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "../provider.h"
#include "../util.h"

/*
 * Mock provider for testing the rendering pipeline without a real LLM.
 *
 * Two modes:
 *
 *  1. Scripted — when HAX_MOCK_SCRIPT points to a file, the mock plays
 *     one turn of directives per stream() call, in file order:
 *
 *       # comments start with '#'
 *       text Looking at this codebase
 *       delay 100
 *       tool bash {"command":"ls -la"}
 *       end-turn
 *
 *       text Reading the main file
 *       tool read {"path":"src/main.c"}
 *       end-turn
 *
 *       text All done.
 *       end-turn
 *
 *     Directives:
 *       text <message>           One text delta with the rest of the line.
 *                                Decodes \n, \t, \\ so a single delta can
 *                                span multiple lines (used by the markdown
 *                                wrap fixtures in mock_layout.txt).
 *       space                    One single-space text delta. Use between
 *                                consecutive `text` directives that should
 *                                read as joined prose; without it, two
 *                                `text` lines emit back-to-back deltas with
 *                                no separator (same shape real providers
 *                                produce token-aligned).
 *       tool <name> <json>       One tool call. Args is a single-line JSON object.
 *       delay <ms>               Sleep before the next emission and between
 *                                auto-streamed text chunks.
 *       usage in=N out=M [cached=K]   Set usage on the upcoming done event.
 *       end-turn                 Finalize the current turn (emits EV_DONE).
 *       (blank or # line)        Ignored.
 *
 *     Long text is auto-streamed in TEXT_CHUNK_BYTES-sized deltas with
 *     `delay` between chunks, so the live preview shows incremental
 *     progress. delay 0 (default) is burst-mode for fast tests.
 *
 *  2. Interactive — when HAX_MOCK_SCRIPT is unset, the mock parses the
 *     latest user message and emits a heuristic response:
 *       - Backtick-quoted text → bash tool call (or read, if the
 *         message starts with "read").
 *       - After a tool result, a brief acknowledgment.
 *       - Anything else → echo the message back as plain text.
 *     Designed so `HAX_PROVIDER=mock hax` works out of the box: the
 *     user types "run `ls -la`" and gets a real bash tool dispatch
 *     they can watch render in the preview.
 */

#define TEXT_CHUNK_BYTES 16

struct mock_provider {
    struct provider base;
    /* NULL → interactive mode. Non-NULL → path to a script file. The
     * path is captured at construction time; subsequent edits to the
     * file are picked up because we re-open it on every stream() call. */
    char *script_path;
    /* Index of the next turn to play in scripted mode. Each completed
     * stream() call increments this; on script exhaustion we fall
     * through to a friendly "no more turns" message. */
    int next_turn;
};

/* Sleep up to `ms` milliseconds, calling `tick` every 50ms so a long
 * scripted delay doesn't pin the REPL and the agent's idle detection
 * fires through the mock the same way it would through libcurl's
 * progress callback. Returns 1 if the tick signals cancel, 0 on full
 * sleep or non-positive `ms`. Real providers reach this path through
 * libcurl's progress callback; the mock has no HTTP so it polls. */
static int mock_tick(http_tick_cb tick, void *tick_user)
{
    return tick ? tick(tick_user) : 0;
}

static int msleep(long ms, http_tick_cb tick, void *tick_user)
{
    while (ms > 0) {
        if (mock_tick(tick, tick_user))
            return 1;
        long step = ms < 50 ? ms : 50;
        struct timespec ts = {step / 1000, (step % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        ms -= step;
    }
    return mock_tick(tick, tick_user);
}

/* Auto-stream `s` as multiple text deltas of up to TEXT_CHUNK_BYTES
 * bytes, sleeping `delay_ms` between chunks. The chunked emission is
 * what gives the live preview something to repaint when delay > 0;
 * with delay == 0 the deltas still arrive separately but back-to-back.
 *
 * Chunks are walked back to a UTF-8 codepoint boundary so a multibyte
 * character (em-dash, emoji, …) never straddles two deltas. Real
 * providers send token-aligned deltas which are already UTF-8-complete;
 * a naive byte-window split would diverge from that and corrupt any
 * mid-stream rendering that interleaves with the partial bytes (the
 * idle inline spinner being the most visible example). */
static int emit_text_chunked(stream_cb cb, void *user, const char *s, long delay_ms,
                             http_tick_cb tick, void *tick_user)
{
    size_t n = strlen(s);
    if (n == 0)
        return 0;
    char buf[TEXT_CHUNK_BYTES + 1];
    size_t i = 0;
    while (i < n) {
        if (mock_tick(tick, tick_user))
            return 0;
        size_t take = (n - i) < TEXT_CHUNK_BYTES ? (n - i) : TEXT_CHUNK_BYTES;
        /* Walk back into the chunk while the proposed cut byte is a
         * UTF-8 continuation (10xxxxxx). Don't cross past the start
         * of the chunk — a single oversized codepoint just emits as-
         * is, no worse than the original byte-sized split would have
         * been, and keeps progress monotone. */
        if (i + take < n) {
            while (take > 0 && (s[i + take] & 0xC0) == 0x80)
                take--;
            if (take == 0)
                take = (n - i) < TEXT_CHUNK_BYTES ? (n - i) : TEXT_CHUNK_BYTES;
        }
        memcpy(buf, s + i, take);
        buf[take] = '\0';
        struct stream_event ev = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = buf}};
        int rc = cb(&ev, user);
        if (rc)
            return rc;
        i += take;
        if (i < n) {
            if (msleep(delay_ms, tick, tick_user))
                return 0;
        }
    }
    return 0;
}

static int emit_tool_call(stream_cb cb, void *user, const char *name, const char *args_json)
{
    char id[37];
    gen_uuid_v4(id);

    struct stream_event start = {.kind = EV_TOOL_CALL_START,
                                 .u.tool_call_start = {.id = id, .name = name}};
    if (cb(&start, user))
        return -1;
    struct stream_event delta = {.kind = EV_TOOL_CALL_DELTA,
                                 .u.tool_call_delta = {.id = id, .args_delta = args_json}};
    if (cb(&delta, user))
        return -1;
    struct stream_event end = {.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = id}};
    return cb(&end, user);
}

static int emit_done(stream_cb cb, void *user, struct stream_usage usage)
{
    struct stream_event ev = {.kind = EV_DONE,
                              .u.done = {.stop_reason = "end_turn", .usage = usage}};
    return cb(&ev, user);
}

static struct stream_usage empty_usage(void)
{
    return (struct stream_usage){.input_tokens = -1, .output_tokens = -1, .cached_tokens = -1};
}

/* ---- Script parsing ----------------------------------------------- */

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int line_is_blank_or_comment(const char *s)
{
    s = skip_ws(s);
    return *s == '\0' || *s == '#';
}

/* Match `prefix` at the start of `s` with a word boundary after it
 * (whitespace or end-of-string). On match, *rest points at the first
 * non-whitespace character past the prefix. */
static int starts_with(const char *s, const char *prefix, const char **rest)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0)
        return 0;
    if (s[n] != ' ' && s[n] != '\t' && s[n] != '\0')
        return 0;
    if (rest)
        *rest = skip_ws(s + n);
    return 1;
}

static long parse_long(const char *s, long fallback)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s)
        return fallback;
    return v;
}

static void strip_eol(char *line)
{
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[n - 1] = '\0';
        n--;
    }
}

/* Parse a "key=N key=N..." usage spec. Unknown keys are silently
 * ignored so the directive can grow without breaking older scripts. */
static struct stream_usage parse_usage(const char *s)
{
    struct stream_usage u = empty_usage();
    while (*s) {
        s = skip_ws(s);
        if (!*s)
            break;
        const char *eq = strchr(s, '=');
        if (!eq)
            break;
        long v = parse_long(eq + 1, -1);
        if (strncmp(s, "in=", 3) == 0 || strncmp(s, "input=", 6) == 0)
            u.input_tokens = v;
        else if (strncmp(s, "out=", 4) == 0 || strncmp(s, "output=", 7) == 0)
            u.output_tokens = v;
        else if (strncmp(s, "cached=", 7) == 0)
            u.cached_tokens = v;
        /* Skip past this token. */
        const char *p = eq + 1;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        s = p;
    }
    return u;
}

/* Read directives from `f` up to and including the next end-turn (or
 * EOF), emitting events as we go. Returns 0 on clean turn, the
 * callback's non-zero return on cancellation, or -2 if EOF was hit
 * with no directives in this turn (caller should treat as "exhausted"). */
static int play_one_turn(FILE *f, stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    char line[8192];
    long delay_ms = 0;
    struct stream_usage usage = empty_usage();
    int saw_directive = 0;

    while (fgets(line, sizeof line, f)) {
        /* Poll the tick between directives so a long script can be cut
         * short cleanly. Emit EV_DONE so the agent can absorb a
         * partial turn the same way it would for a real cancelled
         * stream. */
        if (mock_tick(tick, tick_user))
            return emit_done(cb, user, usage);

        strip_eol(line);
        if (line_is_blank_or_comment(line))
            continue;

        const char *body = skip_ws(line);
        const char *rest;

        if (starts_with(body, "end-turn", NULL))
            return emit_done(cb, user, usage);

        saw_directive = 1;

        if (starts_with(body, "delay", &rest)) {
            delay_ms = parse_long(rest, 0);
            continue;
        }
        if (starts_with(body, "text", &rest)) {
            if (msleep(delay_ms, tick, tick_user))
                return emit_done(cb, user, usage);
            /* Decode minimal C-style escapes (\n, \t, \\) so script
             * lines can embed real newlines without breaking the
             * line-per-directive parser. Anything else passes
             * through; an unrecognized \X is emitted as the literal
             * two bytes so existing scripts that happen to contain a
             * backslash aren't silently mangled. */
            size_t rlen = strlen(rest);
            char *decoded = xmalloc(rlen + 1);
            size_t di = 0;
            for (size_t si = 0; si < rlen; si++) {
                if (rest[si] == '\\' && si + 1 < rlen) {
                    char e = rest[si + 1];
                    if (e == 'n') {
                        decoded[di++] = '\n';
                        si++;
                        continue;
                    }
                    if (e == 't') {
                        decoded[di++] = '\t';
                        si++;
                        continue;
                    }
                    if (e == '\\') {
                        decoded[di++] = '\\';
                        si++;
                        continue;
                    }
                }
                decoded[di++] = rest[si];
            }
            decoded[di] = '\0';
            int rc = emit_text_chunked(cb, user, decoded, delay_ms, tick, tick_user);
            free(decoded);
            if (rc)
                return rc;
            continue;
        }
        if (starts_with(body, "space", NULL)) {
            /* Honor delay_ms before emission, matching `text` / `tool`
             * — `space` is a one-byte text delta, not exempt from the
             * configured pacing. Lets a script use `delay X / space`
             * to model "wait X then emit space" (e.g. a token-aligned
             * space delta arriving after a long pause). */
            if (msleep(delay_ms, tick, tick_user))
                return emit_done(cb, user, usage);
            struct stream_event sp = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = " "}};
            int rc = cb(&sp, user);
            if (rc)
                return rc;
            continue;
        }
        if (starts_with(body, "tool", &rest)) {
            /* "tool <name> <json>" — split off the name at the first
             * whitespace, the rest is the JSON args blob (passed
             * through verbatim). Accept both space and tab so the
             * boundary matches the rest of the parser's whitespace
             * handling. */
            const char *space = strpbrk(rest, " \t");
            if (!space || !*(space + 1)) {
                fprintf(stderr, "hax mock: 'tool' needs name and JSON args: %s\n", line);
                continue;
            }
            size_t name_len = (size_t)(space - rest);
            char *name = xmalloc(name_len + 1);
            memcpy(name, rest, name_len);
            name[name_len] = '\0';
            const char *args = skip_ws(space);
            if (msleep(delay_ms, tick, tick_user)) {
                free(name);
                return emit_done(cb, user, usage);
            }
            int rc = emit_tool_call(cb, user, name, args);
            free(name);
            if (rc)
                return rc;
            continue;
        }
        if (starts_with(body, "usage", &rest)) {
            usage = parse_usage(rest);
            continue;
        }
        fprintf(stderr, "hax mock: unknown directive: %s\n", line);
    }

    /* EOF without explicit end-turn: treat any directives we saw as a
     * final turn, and an empty tail as "script is exhausted". */
    if (saw_directive)
        return emit_done(cb, user, usage);
    return -2;
}

/* Forward `f` past `target` end-turns so the caller can play turn N. */
static int skip_to_turn(FILE *f, int target)
{
    char line[8192];
    int skipped = 0;
    while (skipped < target && fgets(line, sizeof line, f)) {
        strip_eol(line);
        if (starts_with(skip_ws(line), "end-turn", NULL))
            skipped++;
    }
    return skipped;
}

/* ---- Interactive mode --------------------------------------------- */

static const struct item *last_of_kind(const struct context *ctx, enum item_kind kind)
{
    for (size_t i = ctx->n_items; i > 0; i--) {
        if (ctx->items[i - 1].kind == kind)
            return &ctx->items[i - 1];
    }
    return NULL;
}

/* Return the last item that's actually content — skipping any
 * ITEM_TURN_BOUNDARY marker the agent inserts before each new turn,
 * so the heuristic sees the real most-recent thing the user or a
 * tool produced. */
static const struct item *last_item(const struct context *ctx)
{
    for (size_t i = ctx->n_items; i > 0; i--) {
        if (ctx->items[i - 1].kind != ITEM_TURN_BOUNDARY)
            return &ctx->items[i - 1];
    }
    return NULL;
}

/* Extract text between the first pair of backticks. Returns malloc'd
 * or NULL when no balanced pair is present. */
static char *extract_backticks(const char *s)
{
    const char *open = strchr(s, '`');
    if (!open)
        return NULL;
    const char *close = strchr(open + 1, '`');
    if (!close)
        return NULL;
    size_t n = (size_t)(close - open - 1);
    char *out = xmalloc(n + 1);
    memcpy(out, open + 1, n);
    out[n] = '\0';
    return out;
}

/* Crude case-insensitive "starts with one of the verbs" check. Used to
 * route a backtick-quoted argument to the right tool. */
static int starts_with_verb_ci(const char *s, const char *verb)
{
    size_t n = strlen(verb);
    s = skip_ws(s);
    if (strncasecmp(s, verb, n) != 0)
        return 0;
    /* Verb must be followed by a word boundary so "reader" doesn't
     * match "read". */
    return s[n] == ' ' || s[n] == '\t' || s[n] == '`' || s[n] == '\0';
}

/* JSON-escape `s` into `out`. `out` must hold at least 6*strlen(s)+1
 * bytes — every byte may expand to a 6-char `\uXXXX` form for control
 * bytes outside the named escapes. Non-ASCII (>= 0x80) passes through
 * untouched, since the input is already UTF-8 and JSON allows raw
 * UTF-8 inside strings. */
static void json_escape(const char *s, char *out)
{
    static const char hex[] = "0123456789abcdef";
    char *o = out;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':
            *o++ = '\\';
            *o++ = '"';
            break;
        case '\\':
            *o++ = '\\';
            *o++ = '\\';
            break;
        case '\n':
            *o++ = '\\';
            *o++ = 'n';
            break;
        case '\r':
            *o++ = '\\';
            *o++ = 'r';
            break;
        case '\t':
            *o++ = '\\';
            *o++ = 't';
            break;
        case '\b':
            *o++ = '\\';
            *o++ = 'b';
            break;
        case '\f':
            *o++ = '\\';
            *o++ = 'f';
            break;
        default:
            if (c < 0x20) {
                /* All other C0 controls must be escaped per RFC 8259
                 * §7 — emitting them raw would produce invalid JSON
                 * that jansson rejects on the agent side. */
                *o++ = '\\';
                *o++ = 'u';
                *o++ = '0';
                *o++ = '0';
                *o++ = hex[(c >> 4) & 0xF];
                *o++ = hex[c & 0xF];
            } else {
                *o++ = (char)c;
            }
            break;
        }
    }
    *o = '\0';
}

static int interactive_response(const struct context *ctx, stream_cb cb, void *user,
                                http_tick_cb tick, void *tick_user)
{
    const struct item *last = last_item(ctx);
    if (!last)
        return emit_done(cb, user, empty_usage());

    /* Just got a tool result back — keep things terse and end the turn
     * so the user can decide what to do next. */
    if (last->kind == ITEM_TOOL_RESULT) {
        int rc = emit_text_chunked(cb, user, "Tool finished — awaiting next instruction.", 0, tick,
                                   tick_user);
        if (rc)
            return rc;
        return emit_done(cb, user, empty_usage());
    }

    const struct item *u = last_of_kind(ctx, ITEM_USER_MESSAGE);
    if (!u || !u->text || !*u->text) {
        int rc = emit_text_chunked(cb, user, "Hello.", 0, tick, tick_user);
        if (rc)
            return rc;
        return emit_done(cb, user, empty_usage());
    }

    /* The backtick → tool-call shortcut is only useful when tools are
     * actually advertised. Under `--raw` (ctx->n_tools == 0) the agent
     * would refuse to execute anyway, so a tool call would just bounce
     * back as an error — fall through to the echo path so smoke tests
     * of the barebones-chat mode behave predictably. */
    char *quoted = extract_backticks(u->text);
    if (quoted && ctx->n_tools > 0) {
        const char *tool_name = "bash";
        const char *arg_key = "command";
        if (starts_with_verb_ci(u->text, "read")) {
            tool_name = "read";
            arg_key = "path";
        }
        /* json_escape can expand a control byte to 6 chars (\uXXXX). */
        char *escaped = xmalloc(strlen(quoted) * 6 + 1);
        json_escape(quoted, escaped);
        char *args = xasprintf("{\"%s\":\"%s\"}", arg_key, escaped);
        int rc = emit_text_chunked(cb, user, "Sure, on it.", 0, tick, tick_user);
        if (!rc)
            rc = emit_tool_call(cb, user, tool_name, args);
        free(args);
        free(escaped);
        free(quoted);
        if (rc)
            return rc;
        return emit_done(cb, user, empty_usage());
    }
    free(quoted); /* NULL-safe; covers both the no-backticks and --raw paths */

    /* Echo the message back so the user sees something was received.
     * Useful when smoke-testing assistant-text rendering. */
    char *echo = xasprintf("You said: %s", u->text);
    int rc = emit_text_chunked(cb, user, echo, 0, tick, tick_user);
    free(echo);
    if (rc)
        return rc;
    return emit_done(cb, user, empty_usage());
}

/* ---- Provider entry points ---------------------------------------- */

static int mock_stream(struct provider *p, const struct context *ctx, const char *model,
                       stream_cb cb, void *user, http_tick_cb tick, void *tick_user)
{
    (void)model;
    struct mock_provider *m = (struct mock_provider *)p;

    if (!m->script_path)
        return interactive_response(ctx, cb, user, tick, tick_user);

    FILE *f = fopen(m->script_path, "r");
    if (!f) {
        char *msg = xasprintf("mock: cannot open '%s': %s", m->script_path, strerror(errno));
        struct stream_event ev = {.kind = EV_ERROR, .u.error = {.message = msg, .http_status = 0}};
        cb(&ev, user);
        free(msg);
        return -1;
    }

    int skipped = skip_to_turn(f, m->next_turn);
    if (skipped < m->next_turn) {
        fclose(f);
        emit_text_chunked(cb, user, "Script exhausted — no more turns.", 0, tick, tick_user);
        return emit_done(cb, user, empty_usage());
    }

    int rc = play_one_turn(f, cb, user, tick, tick_user);
    fclose(f);

    if (rc == -2) {
        emit_text_chunked(cb, user, "Script exhausted — no more turns.", 0, tick, tick_user);
        return emit_done(cb, user, empty_usage());
    }
    if (rc == 0)
        m->next_turn++;
    return rc;
}

static void mock_destroy(struct provider *p)
{
    struct mock_provider *m = (struct mock_provider *)p;
    free(m->script_path);
    free(m);
}

struct provider *mock_provider_new(void)
{
    struct mock_provider *m = xcalloc(1, sizeof(*m));
    m->base.name = "mock";
    m->base.default_model = "mock-model";
    m->base.stream = mock_stream;
    m->base.destroy = mock_destroy;

    const char *script = getenv("HAX_MOCK_SCRIPT");
    if (script && *script)
        m->script_path = xstrdup(script);

    return &m->base;
}

const struct provider_factory PROVIDER_MOCK = {
    .name = "mock",
    .new = mock_provider_new,
};
