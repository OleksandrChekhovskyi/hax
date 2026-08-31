/* SPDX-License-Identifier: MIT */
#include "providers/mock.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"
#include "providers/registry.h"
#include "system/rand.h"
#include "transport/http.h"

#define TEXT_CHUNK_BYTES 16

struct mock_provider {
    struct provider base;
    char *script_path; /* owned; NULL selects interactive mode */
    size_t next_script_turn;
};

enum script_result {
    SCRIPT_TURN_COMPLETE,
    SCRIPT_EXHAUSTED,
    SCRIPT_ABORTED,
};

enum delta_kind {
    DELTA_TEXT,
    DELTA_REASONING,
};

static int poll_tick(http_tick_cb tick, void *tick_user)
{
    return tick ? tick(tick_user) : 0;
}

/* Sleep `ms` milliseconds, polling `tick` at least once and every 50ms.
 * Real providers stay cancellable through libcurl's progress callback;
 * the mock has no HTTP, so it polls. Returns nonzero when the tick
 * signals cancellation. */
static int msleep(long ms, http_tick_cb tick, void *tick_user)
{
    while (ms > 0) {
        if (poll_tick(tick, tick_user))
            return 1;
        long step = ms < 50 ? ms : 50;
        struct timespec ts = {step / 1000, (step % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        ms -= step;
    }
    return poll_tick(tick, tick_user);
}

/* Keep each delta UTF-8-complete because renderers may process deltas independently. */
static int emit_chunked(stream_cb callback, void *callback_user, const char *text, long delay_ms,
                        http_tick_cb tick, void *tick_user, enum delta_kind kind)
{
    size_t length = strlen(text);
    char chunk[TEXT_CHUNK_BYTES + 1];
    size_t offset = 0;

    while (offset < length) {
        if (poll_tick(tick, tick_user))
            return 1;

        size_t chunk_len = length - offset < TEXT_CHUNK_BYTES ? length - offset : TEXT_CHUNK_BYTES;
        if (offset + chunk_len < length) {
            while (chunk_len > 0 && ((unsigned char)text[offset + chunk_len] & 0xC0) == 0x80)
                chunk_len--;
            /* A full window of continuation bytes is already malformed; preserve progress. */
            if (chunk_len == 0)
                chunk_len = length - offset < TEXT_CHUNK_BYTES ? length - offset : TEXT_CHUNK_BYTES;
        }

        memcpy(chunk, text + offset, chunk_len);
        chunk[chunk_len] = '\0';
        struct stream_event event;
        if (kind == DELTA_REASONING)
            event = (struct stream_event){.kind = EV_REASONING_DELTA,
                                          .u.reasoning_delta = {.text = chunk}};
        else
            event = (struct stream_event){.kind = EV_TEXT_DELTA, .u.text_delta = {.text = chunk}};
        int rc = callback(&event, callback_user);
        if (rc)
            return rc;

        offset += chunk_len;
        if (offset < length && msleep(delay_ms, tick, tick_user))
            return 1;
    }
    return 0;
}

static int emit_text_chunked(stream_cb callback, void *callback_user, const char *text,
                             long delay_ms, http_tick_cb tick, void *tick_user)
{
    return emit_chunked(callback, callback_user, text, delay_ms, tick, tick_user, DELTA_TEXT);
}

static int emit_tool_call(stream_cb callback, void *callback_user, const char *name,
                          const char *args_json, long delay_ms, http_tick_cb tick, void *tick_user)
{
    char id[37];
    gen_uuid_v4(id);

    struct stream_event start = {.kind = EV_TOOL_CALL_START,
                                 .u.tool_call_start = {.id = id, .name = name}};
    int rc = callback(&start, callback_user);
    if (rc)
        return rc;

    /* Expose the composing state that real providers produce while streaming arguments. */
    if (msleep(delay_ms, tick, tick_user))
        return -1;

    struct stream_event delta = {.kind = EV_TOOL_CALL_DELTA,
                                 .u.tool_call_delta = {.id = id, .args_delta = args_json}};
    rc = callback(&delta, callback_user);
    if (rc)
        return rc;

    struct stream_event end = {.kind = EV_TOOL_CALL_END, .u.tool_call_end = {.id = id}};
    return callback(&end, callback_user);
}

static int emit_done(stream_cb callback, void *callback_user, struct stream_usage usage)
{
    struct stream_event event = {.kind = EV_DONE,
                                 .u.done = {.stop_reason = "end_turn", .usage = usage}};
    return callback(&event, callback_user);
}

static struct stream_usage unreported_usage(void)
{
    return (struct stream_usage){.input_tokens = -1,
                                 .output_tokens = -1,
                                 .cached_tokens = -1,
                                 .cache_write_tokens = -1,
                                 .cache_write_1h_tokens = -1,
                                 .cost = -1};
}

/* Scripted mode */

/* {{CWD}} lets checked-in fixtures exercise normalization against the actual working directory. */
static char *expand_cwd(const char *text)
{
    static const char token[] = "{{CWD}}";
    const size_t token_len = sizeof(token) - 1;
    if (!strstr(text, token))
        return xstrdup(text);

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return xstrdup(text);

    struct buf expanded;
    buf_init(&expanded);
    for (const char *cursor = text; *cursor;) {
        if (strncmp(cursor, token, token_len) == 0) {
            buf_append_str(&expanded, cwd);
            cursor += token_len;
        } else {
            buf_append(&expanded, cursor, 1);
            cursor++;
        }
    }
    return buf_steal(&expanded);
}

static const char *skip_ws(const char *text)
{
    while (*text == ' ' || *text == '\t')
        text++;
    return text;
}

static int line_is_blank_or_comment(const char *line)
{
    line = skip_ws(line);
    return *line == '\0' || *line == '#';
}

static int match_directive(const char *line, const char *name, const char **argument)
{
    size_t name_len = strlen(name);
    if (strncmp(line, name, name_len) != 0)
        return 0;
    if (line[name_len] != ' ' && line[name_len] != '\t' && line[name_len] != '\0')
        return 0;
    if (argument)
        *argument = skip_ws(line + name_len);
    return 1;
}

static long parse_long_or(const char *text, long fallback)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || (*end && *end != ' ' && *end != '\t'))
        return fallback;
    return value;
}

static int usage_key_is(const char *start, const char *end, const char *name)
{
    size_t name_len = strlen(name);
    return (size_t)(end - start) == name_len && strncmp(start, name, name_len) == 0;
}

static void strip_eol(char *line)
{
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[n - 1] = '\0';
        n--;
    }
}

/* Unknown usage keys are ignored so scripts remain compatible as accounting grows. */
static struct stream_usage parse_usage(const char *spec)
{
    struct stream_usage usage = unreported_usage();
    while (*spec) {
        spec = skip_ws(spec);
        if (!*spec)
            break;

        const char *token_end = spec;
        while (*token_end && *token_end != ' ' && *token_end != '\t')
            token_end++;

        const char *separator = memchr(spec, '=', (size_t)(token_end - spec));
        if (!separator) {
            spec = token_end;
            continue;
        }

        long value = parse_long_or(separator + 1, -1);
        if (usage_key_is(spec, separator, "in") || usage_key_is(spec, separator, "input"))
            usage.input_tokens = value;
        else if (usage_key_is(spec, separator, "out") || usage_key_is(spec, separator, "output"))
            usage.output_tokens = value;
        else if (usage_key_is(spec, separator, "cached"))
            usage.cached_tokens = value;
        else if (usage_key_is(spec, separator, "cache_write"))
            usage.cache_write_tokens = value;
        else if (usage_key_is(spec, separator, "cache_write_1h"))
            usage.cache_write_1h_tokens = value;
        else if (usage_key_is(spec, separator, "cost")) {
            char *end;
            errno = 0;
            double cost = strtod(separator + 1, &end);
            if (!errno && end == token_end)
                usage.cost = cost;
        }
        spec = token_end;
    }
    return usage;
}

/* Unknown escapes remain literal so fixture paths and JSON are not silently changed. */
static char *decode_escapes(const char *text)
{
    char *decoded = xmalloc(strlen(text) + 1);
    char *output = decoded;
    while (*text) {
        if (text[0] == '\\' && text[1] == 'n') {
            *output++ = '\n';
            text += 2;
        } else if (text[0] == '\\' && text[1] == 't') {
            *output++ = '\t';
            text += 2;
        } else if (text[0] == '\\' && text[1] == '\\') {
            *output++ = '\\';
            text += 2;
        } else {
            *output++ = *text++;
        }
    }
    *output = '\0';
    return decoded;
}

static int emit_script_text(const char *text, enum delta_kind kind, long delay_ms,
                            stream_cb callback, void *callback_user, http_tick_cb tick,
                            void *tick_user)
{
    if (msleep(delay_ms, tick, tick_user))
        return -1;

    char *decoded = decode_escapes(text);
    char *expanded = expand_cwd(decoded);
    free(decoded);
    int rc = emit_chunked(callback, callback_user, expanded, delay_ms, tick, tick_user, kind);
    free(expanded);
    return rc;
}

static int emit_script_tool(const char *spec, const char *line, long delay_ms, stream_cb callback,
                            void *callback_user, http_tick_cb tick, void *tick_user)
{
    const char *name_end = strpbrk(spec, " \t");
    const char *args_json = name_end ? skip_ws(name_end) : NULL;
    if (!name_end || !*args_json) {
        fprintf(stderr, "hax mock: 'tool' needs name and JSON args: %s\n", line);
        return 0;
    }

    size_t name_len = (size_t)(name_end - spec);
    char *name = xmalloc(name_len + 1);
    memcpy(name, spec, name_len);
    name[name_len] = '\0';
    char *expanded_args = expand_cwd(args_json);

    int rc = msleep(delay_ms, tick, tick_user);
    if (!rc)
        rc =
            emit_tool_call(callback, callback_user, name, expanded_args, delay_ms, tick, tick_user);

    free(expanded_args);
    free(name);
    return rc;
}

static enum script_result finish_script_turn(stream_cb callback, void *callback_user,
                                             struct stream_usage usage)
{
    return emit_done(callback, callback_user, usage) ? SCRIPT_ABORTED : SCRIPT_TURN_COMPLETE;
}

static enum script_result play_script_turn(FILE *script, stream_cb callback, void *callback_user,
                                           http_tick_cb tick, void *tick_user)
{
    char line[8192];
    long delay_ms = 0;
    struct stream_usage usage = unreported_usage();
    int saw_directive = 0;

    while (fgets(line, sizeof(line), script)) {
        if (poll_tick(tick, tick_user))
            return SCRIPT_ABORTED;

        strip_eol(line);
        if (line_is_blank_or_comment(line))
            continue;

        const char *directive = skip_ws(line);
        const char *argument;
        if (match_directive(directive, "end-turn", NULL))
            return finish_script_turn(callback, callback_user, usage);

        saw_directive = 1;
        int rc = 0;
        if (match_directive(directive, "delay", &argument)) {
            delay_ms = parse_long_or(argument, 0);
            if (delay_ms < 0)
                delay_ms = 0;
        } else if (match_directive(directive, "text", &argument)) {
            rc = emit_script_text(argument, DELTA_TEXT, delay_ms, callback, callback_user, tick,
                                  tick_user);
        } else if (match_directive(directive, "reasoning", &argument)) {
            rc = emit_script_text(argument, DELTA_REASONING, delay_ms, callback, callback_user,
                                  tick, tick_user);
        } else if (match_directive(directive, "space", NULL)) {
            if (msleep(delay_ms, tick, tick_user))
                rc = -1;
            else {
                struct stream_event event = {.kind = EV_TEXT_DELTA, .u.text_delta = {.text = " "}};
                rc = callback(&event, callback_user);
            }
        } else if (match_directive(directive, "tool", &argument)) {
            rc = emit_script_tool(argument, line, delay_ms, callback, callback_user, tick,
                                  tick_user);
        } else if (match_directive(directive, "usage", &argument)) {
            usage = parse_usage(argument);
        } else {
            fprintf(stderr, "hax mock: unknown directive: %s\n", line);
        }

        if (rc)
            return SCRIPT_ABORTED;
    }

    if (!saw_directive)
        return SCRIPT_EXHAUSTED;
    return finish_script_turn(callback, callback_user, usage);
}

static int skip_script_turns(FILE *script, size_t count)
{
    char line[8192];
    size_t skipped = 0;
    while (skipped < count && fgets(line, sizeof(line), script)) {
        strip_eol(line);
        if (match_directive(skip_ws(line), "end-turn", NULL))
            skipped++;
    }
    return skipped == count ? 0 : -1;
}

/* Interactive mode */

static const struct item *last_of_kind(const struct context *context, enum item_kind kind)
{
    for (size_t i = context->n_items; i > 0; i--) {
        if (context->items[i - 1].kind == kind)
            return &context->items[i - 1];
    }
    return NULL;
}

static const struct item *last_content_item(const struct context *context)
{
    for (size_t i = context->n_items; i > 0; i--) {
        enum item_kind kind = context->items[i - 1].kind;
        if (kind != ITEM_TURN_BOUNDARY && kind != ITEM_TURN_USAGE)
            return &context->items[i - 1];
    }
    return NULL;
}

/* Return allocated text between the first balanced pair, or NULL. */
static char *extract_backtick_text(const char *text)
{
    const char *open = strchr(text, '`');
    if (!open)
        return NULL;
    const char *close = strchr(open + 1, '`');
    if (!close)
        return NULL;

    size_t length = (size_t)(close - open - 1);
    char *quoted = xmalloc(length + 1);
    memcpy(quoted, open + 1, length);
    quoted[length] = '\0';
    return quoted;
}

static int message_starts_with(const char *message, const char *verb)
{
    size_t verb_len = strlen(verb);
    message = skip_ws(message);
    if (strncasecmp(message, verb, verb_len) != 0)
        return 0;
    return message[verb_len] == ' ' || message[verb_len] == '\t' || message[verb_len] == '`' ||
           message[verb_len] == '\0';
}

static int context_has_tool(const struct context *context, const char *name)
{
    for (size_t i = 0; i < context->n_tools; i++) {
        if (strcmp(context->tools[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static char *escape_json_string(const char *text)
{
    static const char hex[] = "0123456789abcdef";
    struct buf escaped;
    buf_init(&escaped);

    for (; *text; text++) {
        unsigned char byte = (unsigned char)*text;
        const char *replacement = NULL;
        switch (byte) {
        case '"':
            replacement = "\\\"";
            break;
        case '\\':
            replacement = "\\\\";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        case '\b':
            replacement = "\\b";
            break;
        case '\f':
            replacement = "\\f";
            break;
        default:
            break;
        }
        if (replacement) {
            buf_append_str(&escaped, replacement);
        } else if (byte < 0x20) {
            /* RFC 8259 requires all remaining C0 controls to use a Unicode escape. */
            char unicode_escape[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0xF]};
            buf_append(&escaped, unicode_escape, sizeof(unicode_escape));
        } else {
            buf_append(&escaped, &byte, 1);
        }
    }
    return buf_steal(&escaped);
}

static int interactive_response(const struct context *context, stream_cb callback,
                                void *callback_user, http_tick_cb tick, void *tick_user)
{
    const struct item *last = last_content_item(context);
    if (!last)
        return emit_done(callback, callback_user, unreported_usage());

    if (last->kind == ITEM_TOOL_RESULT) {
        int rc =
            emit_text_chunked(callback, callback_user, "Tool finished — awaiting next instruction.",
                              0, tick, tick_user);
        if (rc)
            return rc;
        return emit_done(callback, callback_user, unreported_usage());
    }

    const struct item *user_message = last_of_kind(context, ITEM_USER_MESSAGE);
    if (!user_message || !user_message->text || !*user_message->text) {
        int rc = emit_text_chunked(callback, callback_user, "Hello.", 0, tick, tick_user);
        if (rc)
            return rc;
        return emit_done(callback, callback_user, unreported_usage());
    }

    const char *tool_name = message_starts_with(user_message->text, "read") ? "read" : "bash";
    const char *argument_key = strcmp(tool_name, "read") == 0 ? "path" : "command";
    char *quoted = extract_backtick_text(user_message->text);

    /* Raw mode and restricted tool sets must not receive calls the agent will reject. */
    if (quoted && context_has_tool(context, tool_name)) {
        char *escaped = escape_json_string(quoted);
        char *args_json = xasprintf("{\"%s\":\"%s\"}", argument_key, escaped);
        int rc = emit_text_chunked(callback, callback_user, "Sure, on it.", 0, tick, tick_user);
        if (!rc)
            rc = emit_tool_call(callback, callback_user, tool_name, args_json, 0, tick, tick_user);
        free(args_json);
        free(escaped);
        free(quoted);
        if (rc)
            return rc;
        return emit_done(callback, callback_user, unreported_usage());
    }
    free(quoted);

    char *echo = xasprintf("You said: %s", user_message->text);
    int rc = emit_text_chunked(callback, callback_user, echo, 0, tick, tick_user);
    free(echo);
    if (rc)
        return rc;
    return emit_done(callback, callback_user, unreported_usage());
}

/* Provider interface */

static int emit_script_exhausted(stream_cb callback, void *callback_user, http_tick_cb tick,
                                 void *tick_user)
{
    int rc = emit_text_chunked(callback, callback_user, "Script exhausted — no more turns.", 0,
                               tick, tick_user);
    if (rc)
        return rc;
    return emit_done(callback, callback_user, unreported_usage());
}

static int mock_stream(struct provider *provider, const struct context *context, const char *model,
                       stream_cb callback, void *callback_user, http_tick_cb tick, void *tick_user)
{
    (void)model;
    struct mock_provider *mock = (struct mock_provider *)provider;

    if (!mock->script_path)
        return interactive_response(context, callback, callback_user, tick, tick_user);

    FILE *script = fopen(mock->script_path, "r");
    if (!script) {
        char *message = xasprintf("mock: cannot open '%s': %s", mock->script_path, strerror(errno));
        struct stream_event event = {.kind = EV_ERROR,
                                     .u.error = {.message = message, .http_status = 0}};
        callback(&event, callback_user);
        free(message);
        return -1;
    }

    if (skip_script_turns(script, mock->next_script_turn)) {
        fclose(script);
        return emit_script_exhausted(callback, callback_user, tick, tick_user);
    }

    enum script_result result = play_script_turn(script, callback, callback_user, tick, tick_user);
    fclose(script);

    if (result == SCRIPT_EXHAUSTED)
        return emit_script_exhausted(callback, callback_user, tick, tick_user);
    if (result == SCRIPT_ABORTED)
        return -1;

    mock->next_script_turn++;
    return 0;
}

static void mock_destroy(struct provider *provider)
{
    struct mock_provider *mock = (struct mock_provider *)provider;
    model_meta_release(provider);
    free(mock->script_path);
    free(mock);
}

struct provider *mock_provider_new(const struct provider_def *def)
{
    struct mock_provider *mock = xcalloc(1, sizeof(*mock));
    mock->base.name = "mock";
    mock->base.id = def->id;
    mock->base.default_model = "mock-model";
    mock->base.stream = mock_stream;
    mock->base.destroy = mock_destroy;

    const char *script = config_str("providers.mock.script");
    if (script && *script)
        mock->script_path = xstrdup(script);

    return &mock->base;
}
