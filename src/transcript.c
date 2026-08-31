/* SPDX-License-Identifier: MIT */
#include "transcript.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_usage.h"
#include "config.h"
#include "diag.h"
#include "provider.h"
#include "tool_schema.h"
#include "xalloc.h"
#include "render/diff_color.h"
#include "system/locale.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"
#include "text/fmt.h"
#include "tools/output_cap.h"

#define TRANSCRIPT_WIDTH_COLUMNS 60

struct transcript_renderer {
    FILE *out;
    enum transcript_render_mode mode;
};

static const char *ansi(const struct transcript_renderer *renderer, const char *sequence)
{
    return renderer->mode == TRANSCRIPT_RENDER_ANSI ? sequence : "";
}

/* Decoration is the part hax can give up when the system offers no UTF-8 locale at all. The
 * conversation body cannot be, so the pager is handed a charset rather than trusted to have one. */
static const char *glyph(const char *utf8, const char *ascii)
{
    return locale_have_utf8() ? utf8 : ascii;
}

static void render_section_rule(const struct transcript_renderer *renderer, const char *label)
{
    const char *rule = glyph("──", "--");
    fprintf(renderer->out, "%s%s %s %s%s\n", ansi(renderer, ANSI_DIM), rule, label, rule,
            ansi(renderer, ANSI_RESET));
}

static void repeat_glyph(FILE *out, const char *glyph, int count)
{
    for (int i = 0; i < count; i++)
        fputs(glyph, out);
}

static void render_banner(const struct transcript_renderer *renderer)
{
    FILE *out = renderer->out;
    const char *label = "TRANSCRIPT";
    int label_width = (int)strlen(label);
    int inner_width = TRANSCRIPT_WIDTH_COLUMNS - 2;
    int left_padding = (inner_width - label_width) / 2;
    int right_padding = inner_width - label_width - left_padding;

    fputs(ansi(renderer, ANSI_BOLD), out);
    fputs(ansi(renderer, theme_open(THEME_CHROME)), out);
    fputs(glyph("┏", "+"), out);
    repeat_glyph(out, glyph("━", "-"), inner_width);
    fputs(glyph("┓", "+"), out);
    fputs(ansi(renderer, ANSI_RESET), out);
    fputc('\n', out);

    fputs(ansi(renderer, ANSI_BOLD), out);
    fputs(ansi(renderer, theme_open(THEME_CHROME)), out);
    fputs(glyph("┃", "|"), out);
    repeat_glyph(out, " ", left_padding);
    fputs(label, out);
    repeat_glyph(out, " ", right_padding);
    fputs(glyph("┃", "|"), out);
    fputs(ansi(renderer, ANSI_RESET), out);
    fputc('\n', out);

    fputs(ansi(renderer, ANSI_BOLD), out);
    fputs(ansi(renderer, theme_open(THEME_CHROME)), out);
    fputs(glyph("┗", "+"), out);
    repeat_glyph(out, glyph("━", "-"), inner_width);
    fputs(glyph("┛", "+"), out);
    fputs(ansi(renderer, ANSI_RESET), out);
    fputs("\n\n", out);
}

static void render_turn_rule(const struct transcript_renderer *renderer, int turn_number)
{
    FILE *out = renderer->out;
    char label[32];
    int label_width = snprintf(label, sizeof(label), " # turn %d ", turn_number);
    int rule_width = TRANSCRIPT_WIDTH_COLUMNS - label_width;
    if (rule_width < 4)
        rule_width = 4;
    int left_width = rule_width / 2;
    int right_width = rule_width - left_width;

    fputs(ansi(renderer, ANSI_BOLD), out);
    fputs(ansi(renderer, theme_open(THEME_CHROME)), out);
    repeat_glyph(out, glyph("─", "-"), left_width);
    fputs(label, out);
    repeat_glyph(out, glyph("─", "-"), right_width);
    fputs(ansi(renderer, ANSI_RESET), out);
    fputs("\n\n", out);
}

static void ensure_newline(FILE *out, const char *text)
{
    size_t length = text ? strlen(text) : 0;
    if (length == 0 || text[length - 1] != '\n')
        fputc('\n', out);
}

/* less -R resets SGR state at line breaks, so reopen styling on every line. */
static void render_styled_lines(const struct transcript_renderer *renderer, const char *text,
                                const char *open, const char *close)
{
    FILE *out = renderer->out;
    const char *line = text;
    while (*line) {
        const char *newline = strchr(line, '\n');
        size_t line_length = newline ? (size_t)(newline - line) : strlen(line);
        if (renderer->mode == TRANSCRIPT_RENDER_ANSI)
            fputs(open, out);
        fwrite(line, 1, line_length, out);
        if (renderer->mode == TRANSCRIPT_RENDER_ANSI)
            fputs(close, out);
        if (!newline)
            break;
        fputc('\n', out);
        line = newline + 1;
    }
    ensure_newline(out, text);
}

static const char *synthetic_user_label(enum item_origin origin)
{
    if (origin == ITEM_ORIGIN_COMPACT_SEED)
        return "compaction seed";
    if (origin == ITEM_ORIGIN_TASK_NOTE)
        return "task update";
    return "continuation";
}

static void render_user(const struct transcript_renderer *renderer, const struct item *item)
{
    const char *text = item->text ? item->text : "";
    if (item->origin != ITEM_ORIGIN_NONE) {
        render_section_rule(renderer, synthetic_user_label(item->origin));
        render_styled_lines(renderer, text, ANSI_DIM, ANSI_RESET);
        return;
    }

    render_section_rule(renderer, "user");
    render_styled_lines(renderer, text, theme_open(THEME_ACCENT), theme_close(THEME_ACCENT));
}

static void render_assistant(const struct transcript_renderer *renderer, const struct item *item)
{
    FILE *out = renderer->out;
    render_section_rule(renderer, "assistant");
    if (item->text)
        fputs(item->text, out);
    ensure_newline(out, item->text);
}

static void render_json_or_text(FILE *out, const char *text)
{
    json_t *root = json_loads(text, 0, NULL);
    char *pretty = root ? json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER) : NULL;

    if (pretty) {
        fputs(pretty, out);
        ensure_newline(out, pretty);
    } else {
        fputs(text, out);
        ensure_newline(out, text);
    }

    free(pretty);
    if (root)
        json_decref(root);
}

static void render_tool_call(const struct transcript_renderer *renderer, const struct item *item)
{
    FILE *out = renderer->out;
    fprintf(out, "%s[%s]%s\n", ansi(renderer, theme_open(THEME_CHROME)),
            item->tool_name ? item->tool_name : "?", ansi(renderer, ANSI_RESET));

    if (item->tool_arguments_json && *item->tool_arguments_json)
        render_json_or_text(out, item->tool_arguments_json);
}

static void render_tool_schema(FILE *out, const struct tool_def *tool)
{
    json_t *schema = tool_schema_build(tool);
    char *pretty = json_dumps(schema, JSON_INDENT(2));
    json_decref(schema);
    if (!pretty)
        return;
    fputc('\n', out);
    fputs(pretty, out);
    ensure_newline(out, pretty);
    free(pretty);
}

static void render_tools(const struct transcript_renderer *renderer, const struct tool_def *tools,
                         size_t n_tools)
{
    FILE *out = renderer->out;
    if (!tools || n_tools == 0)
        return;

    render_section_rule(renderer, "tools");
    for (size_t i = 0; i < n_tools; i++) {
        fprintf(out, "%s[%s]%s\n", ansi(renderer, theme_open(THEME_CHROME)),
                tools[i].name ? tools[i].name : "?", ansi(renderer, ANSI_RESET));
        if (tools[i].description) {
            fputs(tools[i].description, out);
            ensure_newline(out, tools[i].description);
        }
        render_tool_schema(out, &tools[i]);
        if (i + 1 < n_tools)
            fputc('\n', out);
    }
    fputc('\n', out);
}

static size_t read_line_prefix_length(const char *line, size_t line_length)
{
    size_t offset = 0;
    while (offset < line_length && line[offset] == ' ')
        offset++;

    size_t digits_start = offset;
    while (offset < line_length && line[offset] >= '0' && line[offset] <= '9')
        offset++;
    if (offset == digits_start)
        return 0;

    size_t delimiter_length = strlen(READ_LINE_DELIM);
    if (offset + delimiter_length > line_length ||
        memcmp(line + offset, READ_LINE_DELIM, delimiter_length) != 0)
        return 0;
    return offset + delimiter_length;
}

static void render_read_body(FILE *out, const char *text)
{
    const char *line = text;
    while (*line) {
        const char *newline = strchr(line, '\n');
        size_t line_length = newline ? (size_t)(newline - line) : strlen(line);
        size_t prefix_length = read_line_prefix_length(line, line_length);
        if (prefix_length > 0) {
            fputs(ANSI_DIM, out);
            fwrite(line, 1, prefix_length, out);
            fputs(ANSI_RESET, out);
            fwrite(line + prefix_length, 1, line_length - prefix_length, out);
        } else {
            fwrite(line, 1, line_length, out);
        }
        if (!newline)
            break;
        fputc('\n', out);
        line = newline + 1;
    }
    ensure_newline(out, text);
}

static void render_diff_body(FILE *out, const char *text)
{
    int in_hunk = 0;
    const char *line = text;
    while (*line) {
        const char *newline = strchr(line, '\n');
        size_t line_length = newline ? (size_t)(newline - line) : strlen(line);
        const char *open = NULL;
        switch (diff_line_classify(line, line_length, in_hunk)) {
        case DIFF_LINE_ADD:
            open = theme_open(THEME_ADD);
            break;
        case DIFF_LINE_REMOVE:
            open = theme_open(THEME_REMOVE);
            break;
        case DIFF_LINE_META:
            open = ANSI_DIM;
            break;
        case DIFF_LINE_CONTEXT:
            break;
        }
        if (open)
            fputs(open, out);
        fwrite(line, 1, line_length, out);
        if (open)
            fputs(ANSI_RESET, out);
        if (line_length >= 2 && memcmp(line, "@@", 2) == 0)
            in_hunk = 1;
        if (!newline)
            break;
        fputc('\n', out);
        line = newline + 1;
    }
    ensure_newline(out, text);
}

/* Inline base64 would overwhelm a text transcript, so render image metadata instead. */
static void render_result_images(const struct transcript_renderer *renderer,
                                 const struct item *item)
{
    FILE *out = renderer->out;
    for (size_t i = 0; i < item->n_images; i++) {
        char *placeholder = item_image_placeholder(&item->images[i]);
        fprintf(out, "%s%s%s\n", ansi(renderer, ANSI_DIM), placeholder, ansi(renderer, ANSI_RESET));
        free(placeholder);
    }
}

/* Results do not store their tool name; it is supplied by the paired call when available. */
static void render_tool_result(const struct transcript_renderer *renderer, const struct item *item,
                               const char *tool_name)
{
    FILE *out = renderer->out;
    const char *text = item->output ? item->output : "";
    render_section_rule(renderer, "tool result");
    render_result_images(renderer, item);
    if (renderer->mode == TRANSCRIPT_RENDER_ANSI && tool_name) {
        if (strcmp(tool_name, "read") == 0) {
            render_read_body(out, text);
            return;
        }
        if ((strcmp(tool_name, "edit") == 0 || strcmp(tool_name, "write") == 0) &&
            strncmp(text, "--- ", 4) == 0) {
            render_diff_body(out, text);
            return;
        }
    }
    if (item->output)
        fputs(item->output, out);
    ensure_newline(out, item->output);
}

static void render_reasoning(const struct transcript_renderer *renderer, const struct item *item)
{
    FILE *out = renderer->out;
    if (item->reasoning_text) {
        render_section_rule(renderer, "reasoning");
        render_styled_lines(renderer, item->reasoning_text, ANSI_DIM, ANSI_RESET);
        return;
    }

    fprintf(out, "%s[reasoning]%s", ansi(renderer, ANSI_DIM), ansi(renderer, ANSI_RESET));
    if (item->reasoning_json) {
        json_t *reasoning = json_loads(item->reasoning_json, 0, NULL);
        if (reasoning) {
            const char *id = json_string_value(json_object_get(reasoning, "id"));
            if (id)
                fprintf(out, " %s%s%s", ansi(renderer, ANSI_DIM), id, ansi(renderer, ANSI_RESET));
            json_decref(reasoning);
        }
    }
    fputc('\n', out);
}

static void render_usage_separator(FILE *out, int *is_first)
{
    if (!*is_first)
        fputs(glyph(" · ", " | "), out);
    *is_first = 0;
}

static void render_token_usage(FILE *out, int *is_first, const char *label, long tokens,
                               double cost)
{
    char token_count[32];
    render_usage_separator(out, is_first);
    format_tokens(token_count, sizeof(token_count), tokens);
    fprintf(out, "%s %s", label, token_count);
    if (cost >= COST_DISPLAY_MIN) {
        char formatted_cost[32];
        format_cost(formatted_cost, sizeof(formatted_cost), cost);
        fprintf(out, " ~%s", formatted_cost);
    }
}

/* Debugging detail rather than the transcript's subject, so it stays dim and below the numbers. */
static void render_turn_provenance(const struct transcript_renderer *renderer,
                                   const struct item *item)
{
    const struct turn_provenance *provenance = &item->usage->provenance;
    const char *provider = provenance->provider_label ? provenance->provider_label : item->provider;
    const char *model = provenance->model_label ? provenance->model_label : item->model;
    if ((!provider || !*provider) && (!model || !*model))
        return;

    FILE *out = renderer->out;
    fputs(ansi(renderer, ANSI_DIM), out);
    /* Spelled as the banner spells it, so the same triple is recognizable in both. */
    if (provider && *provider)
        fputs(provider, out);
    if (model && *model) {
        if (provider && *provider)
            fputs(glyph(" · ", " | "), out);
        fputs(model, out);
        if (provenance->effort)
            fprintf(out, "%s%s", glyph(" · ", " | "), provenance->effort);
    }

    /* The arrow introduces the response side unconditionally: a bare "via" trailing the effort
     * level would read as part of it. */
    if (provenance->served_model || provenance->route) {
        fputs(glyph(" → ", " -> "), out);
        fputs(provenance->served_model ? provenance->served_model : provenance->route, out);
        if (provenance->served_model && provenance->route)
            fprintf(out, " via %s", provenance->route);
    }
    fputs(ansi(renderer, ANSI_RESET), out);
    fputc('\n', out);
}

static void render_turn_usage(const struct transcript_renderer *renderer,
                              const struct turn_usage *turn_usage)
{
    FILE *out = renderer->out;
    const struct stream_usage *usage = &turn_usage->usage;
    int is_first = 1;
    char formatted_value[32];

    fputs(ansi(renderer, ANSI_DIM), out);
    if (turn_usage->elapsed_ms >= 0) {
        format_duration(formatted_value, sizeof(formatted_value), turn_usage->elapsed_ms);
        render_usage_separator(out, &is_first);
        fputs(formatted_value, out);
    }
    if (turn_usage->cost_total > 0) {
        format_cost(formatted_value, sizeof(formatted_value), turn_usage->cost_total);
        render_usage_separator(out, &is_first);
        fprintf(out, "%s%s", turn_usage->cost_estimated ? "~" : "", formatted_value);
    }

    long cached_tokens = usage->cached_tokens > 0 ? usage->cached_tokens : 0;
    long cache_write_tokens = usage->cache_write_tokens > 0 ? usage->cache_write_tokens : 0;
    if (usage->input_tokens >= 0)
        render_token_usage(out, &is_first, "in",
                           turn_usage->uncached_input_tokens > 0 ? turn_usage->uncached_input_tokens
                                                                 : 0,
                           turn_usage->cost_input);
    if (cached_tokens > 0)
        render_token_usage(out, &is_first, "cache", cached_tokens, turn_usage->cost_cache_read);
    if (cache_write_tokens > 0)
        render_token_usage(out, &is_first, "write", cache_write_tokens,
                           turn_usage->cost_cache_write);
    if (usage->output_tokens >= 0)
        render_token_usage(out, &is_first, "out", usage->output_tokens, turn_usage->cost_output);
    fputs(ansi(renderer, ANSI_RESET), out);
    fputc('\n', out);
}

void transcript_render_header(FILE *out, enum transcript_render_mode mode,
                              const char *system_prompt, const struct tool_def *tools,
                              size_t n_tools)
{
    const struct transcript_renderer renderer = {.out = out, .mode = mode};
    render_banner(&renderer);

    if (system_prompt && *system_prompt) {
        render_section_rule(&renderer, "system prompt");
        fputs(system_prompt, out);
        ensure_newline(out, system_prompt);
        fputc('\n', out);
    }

    render_tools(&renderer, tools, n_tools);
}

static size_t find_tool_result(const struct item *items, size_t n_items, size_t first_result,
                               const char *call_id)
{
    for (size_t i = first_result; i < n_items; i++)
        if (items[i].kind == ITEM_TOOL_RESULT && items[i].call_id &&
            strcmp(items[i].call_id, call_id) == 0)
            return i;
    return n_items;
}

void transcript_render_items(FILE *out, enum transcript_render_mode mode, const struct item *items,
                             size_t n_items, size_t first_item, int *turn_number)
{
    if (first_item >= n_items)
        return;

    const struct transcript_renderer renderer = {.out = out, .mode = mode};
    char *result_rendered = xcalloc(n_items - first_item, 1);

    for (size_t i = first_item; i < n_items; i++) {
        const struct item *item = &items[i];
        if (item->kind == ITEM_TOOL_RESULT && result_rendered[i - first_item])
            continue;

        switch (item->kind) {
        case ITEM_TURN_BOUNDARY:
            render_turn_rule(&renderer, ++(*turn_number));
            continue;
        case ITEM_USER_MESSAGE:
            render_user(&renderer, item);
            break;
        case ITEM_ASSISTANT_MESSAGE:
            render_assistant(&renderer, item);
            break;
        case ITEM_TOOL_CALL:
            render_tool_call(&renderer, item);
            if (item->call_id) {
                size_t result = find_tool_result(items, n_items, i + 1, item->call_id);
                if (result < n_items) {
                    fputc('\n', out);
                    render_tool_result(&renderer, &items[result], item->tool_name);
                    result_rendered[result - first_item] = 1;
                }
            }
            break;
        case ITEM_TOOL_RESULT:
            render_tool_result(&renderer, item, NULL);
            break;
        case ITEM_REASONING:
            render_reasoning(&renderer, item);
            break;
        case ITEM_TURN_USAGE:
            if (!item->usage)
                continue;
            render_turn_usage(&renderer, item->usage);
            render_turn_provenance(&renderer, item);
            break;
        }
        fputc('\n', out);
    }

    free(result_rendered);
}

void transcript_render(FILE *out, const char *system_prompt, const struct tool_def *tools,
                       size_t n_tools, const struct item *items, size_t n_items)
{
    transcript_render_header(out, TRANSCRIPT_RENDER_ANSI, system_prompt, tools, n_tools);
    int turn_number = 0;
    transcript_render_items(out, TRANSCRIPT_RENDER_ANSI, items, n_items, 0, &turn_number);
}

struct transcript_log {
    FILE *stream;
    char *path; /* owned; config storage may be replaced while the log is open */
    size_t items_written;
    int turn_number;
};

void transcript_log_init(void)
{
    const char *path = config_str("transcript");
    if (!path || !*path)
        return;

    /* The header is written later, once the session configuration is available. */
    FILE *stream = fopen(path, "we");
    if (stream)
        fclose(stream);
}

struct transcript_log *transcript_log_open(const char *system_prompt, const struct tool_def *tools,
                                           size_t n_tools)
{
    const char *path = config_str("transcript");
    if (!path || !*path)
        return NULL;
    FILE *stream = fopen(path, "we");
    if (!stream) {
        hax_warn("HAX_TRANSCRIPT: cannot open '%s' for writing", path);
        return NULL;
    }

    /* Make completed lines immediately visible to readers such as tail -f. */
    setvbuf(stream, NULL, _IOLBF, 0);
    struct transcript_log *log = xmalloc(sizeof(*log));
    log->stream = stream;
    log->path = xstrdup(path);
    log->items_written = 0;
    log->turn_number = 0;
    transcript_render_header(stream, TRANSCRIPT_RENDER_PLAIN, system_prompt, tools, n_tools);
    return log;
}

void transcript_log_append(struct transcript_log *log, const struct item *items, size_t n_items)
{
    if (!log || !log->stream || n_items <= log->items_written)
        return;

    transcript_render_items(log->stream, TRANSCRIPT_RENDER_PLAIN, items, n_items,
                            log->items_written, &log->turn_number);
    log->items_written = n_items;
}

void transcript_log_reset(struct transcript_log *log, const char *system_prompt,
                          const struct tool_def *tools, size_t n_tools)
{
    if (!log)
        return;
    /* freopen closes the old stream on failure; a later reset must recover with fopen. */
    FILE *stream = log->stream ? freopen(log->path, "we", log->stream) : fopen(log->path, "we");
    if (!stream) {
        hax_warn("HAX_TRANSCRIPT: cannot truncate '%s' on /new", log->path);
        log->stream = NULL;
        return;
    }

    log->stream = stream;
    /* Stream buffering does not portably survive freopen. */
    setvbuf(stream, NULL, _IOLBF, 0);
    log->items_written = 0;
    log->turn_number = 0;
    transcript_render_header(stream, TRANSCRIPT_RENDER_PLAIN, system_prompt, tools, n_tools);
}

void transcript_log_close(struct transcript_log *log)
{
    if (!log)
        return;
    if (log->stream)
        fclose(log->stream);
    free(log->path);
    free(log);
}
