/* SPDX-License-Identifier: MIT */
#include "tools/bash_classify.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

/* Formatting filters require an upstream producer; unknown commands, shell side effects, and
 * recognized writer options reject the exploration classification. */
enum command_class {
    COMMAND_EXPLORATION,
    COMMAND_FILTER,
    COMMAND_UNKNOWN,
};

/* Exact basename matching prevents commands with shared prefixes from entering an allowlist. */
static int name_in_list(const char *const *names, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(names[i], name) == 0)
            return 1;
    return 0;
}

#define NAME_IN_LIST(names, name) name_in_list(names, sizeof(names) / sizeof(*(names)), (name))

static int is_read_command(const char *command)
{
    static const char *const names[] = {"cat", "less", "more", "nl"};
    return NAME_IN_LIST(names, command);
}

static int is_list_command(const char *command)
{
    /* `env` is a neutral prefix; `wc` is a filter unless it has a file operand. */
    static const char *const names[] = {
        "ls",  "eza",      "exa",      "tree",     "find",    "fd",       "stat",    "file",
        "pwd", "realpath", "readlink", "which",    "whereis", "basename", "dirname", "du",
        "df",  "id",       "whoami",   "hostname", "uname",   "true",     "false",
    };
    return NAME_IN_LIST(names, command);
}

static int is_search_command(const char *command)
{
    static const char *const names[] = {"grep", "egrep", "fgrep", "rg", "ag", "ack"};
    return NAME_IN_LIST(names, command);
}

/* These commands are filters without a file operand and readers with one. Writers and unbounded
 * producers are intentionally absent. */
static int is_format_filter(const char *command)
{
    static const char *const names[] = {
        "wc",  "sort", "uniq",   "cut",      "tr",    "awk",  "sed",  "head", "tail",   "tac",
        "rev", "fold", "expand", "unexpand", "paste", "comm", "join", "echo", "printf", "column",
    };
    return NAME_IN_LIST(names, command);
}

/* Non-flag operands to these filters are content, not file paths. */
static int is_content_filter(const char *command)
{
    static const char *const names[] = {"echo", "printf", "tr"};
    return NAME_IN_LIST(names, command);
}

/* For commands that may read stdin, `value_options` names options consuming the next token and
 * `required_operands` is the source count required to classify the command as a reader. Separated
 * long-option values are treated as operands by this approximation. */
struct command_spec {
    const char *name;
    const char *value_options;
    int required_operands;
};

static const struct command_spec COMMAND_SPECS[] = {
    /* Readers requiring a file. */
    {"cat", "", 1},
    {"less", "joP", 1},
    {"more", "n", 1},
    {"nl", "bdfhilnpsvw", 1},
    /* grep requires a pattern and file; the others search cwd by default. */
    {"grep", "ABCDdmef", 2},
    {"egrep", "ABCDdmef", 2},
    {"fgrep", "ABCDdmef", 2},
    {"rg", "ABCmtg", 1},
    {"ag", "ABCm", 1},
    {"ack", "ABCm", 1},
    {"head", "nc", 1},
    {"tail", "nc", 1},
    {"wc", "", 1},
    {"sort", "kStTo", 1},
    {"uniq", "fsw", 1},
    {"cut", "bcdf", 1},
    /* The script is the first operand; a source file is required as the second. */
    {"sed", "", 2},
    {"awk", "", 2},
    {"tac", "s", 1},
    {"rev", "", 1},
    {"fold", "w", 1},
    {"expand", "t", 1},
    {"unexpand", "t", 1},
    {"paste", "d", 1},
    {"comm", "", 1},
    {"join", "12teoav", 2},
    {"column", "csoN", 1},
};

static const struct command_spec *command_spec_for(const char *name)
{
    for (size_t i = 0; i < sizeof(COMMAND_SPECS) / sizeof(COMMAND_SPECS[0]); i++) {
        if (strcmp(COMMAND_SPECS[i].name, name) == 0)
            return &COMMAND_SPECS[i];
    }
    return NULL;
}

/* Only explicitly read-only git subcommands qualify for a collapsed preview. */
static enum command_class classify_command(const char *leader, const char *subcommand)
{
    if (is_read_command(leader) || is_search_command(leader) || is_list_command(leader))
        return COMMAND_EXPLORATION;
    if (strcmp(leader, "git") == 0 && subcommand &&
        (strcmp(subcommand, "grep") == 0 || strcmp(subcommand, "ls-files") == 0))
        return COMMAND_EXPLORATION;
    return COMMAND_UNKNOWN;
}

static int is_command_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_word_end(char c)
{
    return c == '\0' || is_command_whitespace(c);
}

static const char *skip_whitespace(const char *cursor, const char *end)
{
    while (cursor < end && is_command_whitespace(*cursor))
        cursor++;
    return cursor;
}

/* Returns an owned, unquoted word and advances `next_out`, or NULL at end of input. */
static char *take_word(const char *cursor, const char *end, const char **next_out)
{
    cursor = skip_whitespace(cursor, end);
    if (cursor >= end) {
        *next_out = cursor;
        return NULL;
    }

    struct buf word;
    buf_init(&word);
    char quote = 0;
    while (cursor < end) {
        char c = *cursor;
        if (quote) {
            if (c == quote) {
                quote = 0;
                cursor++;
                continue;
            }
            if (quote == '"' && c == '\\' && cursor + 1 < end) {
                /* Exact POSIX escape semantics are unnecessary for this display heuristic. */
                buf_append(&word, cursor + 1, 1);
                cursor += 2;
                continue;
            }
            buf_append(&word, &c, 1);
            cursor++;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            cursor++;
            continue;
        }
        if (c == '\\' && cursor + 1 < end) {
            buf_append(&word, cursor + 1, 1);
            cursor += 2;
            continue;
        }
        if (is_word_end(c))
            break;
        buf_append(&word, &c, 1);
        cursor++;
    }
    *next_out = cursor;
    if (word.len == 0) {
        buf_free(&word);
        return NULL;
    }
    char nul = 0;
    buf_append(&word, &nul, 1);
    return buf_steal(&word);
}

/* Separated long-option values count as operands because their grammar is command-specific. */
static int count_operands(const struct command_spec *spec, const char *body,
                          const char *segment_end)
{
    const char *cursor = body;
    char *command = take_word(cursor, segment_end, &cursor);
    free(command);

    int count = 0;
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            break;
        if (token[0] == '-' && token[1] != '\0' && token[1] != '-') {
            int consume_next = 0;
            for (size_t i = 1; token[i]; i++) {
                if (strchr(spec->value_options, token[i])) {
                    consume_next = token[i + 1] == '\0';
                    break;
                }
            }
            free(token);
            if (consume_next) {
                char *value = take_word(cursor, segment_end, &cursor);
                free(value);
            }
            continue;
        }
        if (token[0] == '-' && token[1] == '-' && token[2] != '\0') {
            free(token);
            continue;
        }
        free(token);
        count++;
    }
    return count;
}

static size_t allowed_stderr_redirect_end(const char *command, size_t redirect_offset)
{
    int has_stderr_prefix =
        redirect_offset >= 1 && command[redirect_offset - 1] == '2' &&
        (redirect_offset == 1 || strchr(" \t\n\r&|;", command[redirect_offset - 2]) != NULL);
    if (!has_stderr_prefix)
        return 0;
    if (command[redirect_offset + 1] == '&' && command[redirect_offset + 2] == '1')
        return redirect_offset + 3;

    const char *target = command + redirect_offset + 1;
    while (*target == ' ' || *target == '\t')
        target++;
    if (strncmp(target, "/dev/null", 9) == 0)
        return (size_t)(target + 9 - command);
    return 0;
}

/* This lexical check may reject safe commands but must not hide obvious shell side effects. */
static int command_has_disqualifier(const char *command)
{
    char quote = 0;
    for (size_t offset = 0; command[offset]; offset++) {
        char c = command[offset];
        if (quote) {
            /* Backslashes are literal inside POSIX single quotes. */
            if (quote == '"' && c == '\\' && command[offset + 1]) {
                offset++;
                continue;
            }
            if (c == quote) {
                quote = 0;
                continue;
            }
            if (quote == '"' && (c == '`' || (c == '$' && command[offset + 1] == '(')))
                return 1;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '\\' && command[offset + 1]) {
            offset++;
            continue;
        }
        if (c == '`' || (c == '$' && command[offset + 1] == '('))
            return 1;
        if ((c == '<' || c == '>') && command[offset + 1] == '(')
            return 1;
        if (c == '<' && command[offset + 1] == '<')
            return 1;
        if (c == '&' && command[offset + 1] != '&' &&
            (offset == 0 || (command[offset - 1] != '>' && command[offset - 1] != '&')))
            return 1;
        if (c == '>') {
            size_t redirect_end = allowed_stderr_redirect_end(command, offset);
            if (!redirect_end)
                return 1;
            offset = redirect_end - 1;
        }
    }
    return 0;
}

static int is_variable_assignment(const char *word)
{
    return strchr(word, '=') != NULL && (word[0] == '_' || isalpha((unsigned char)word[0]));
}

/* `neutral_only_out` distinguishes a consumed prefix from an empty segment. */
static const char *skip_neutral_prefix(const char *cursor, const char *end, int *neutral_only_out)
{
    *neutral_only_out = 0;
    int consumed_prefix = 0;
    for (;;) {
        const char *next = cursor;
        char *command = take_word(next, end, &next);
        if (!command) {
            *neutral_only_out = consumed_prefix;
            return NULL;
        }

        int is_neutral = 0;
        if (strcmp(command, "cd") == 0 || strcmp(command, "pushd") == 0) {
            const char *argument_cursor = next;
            for (;;) {
                char *argument = take_word(argument_cursor, end, &argument_cursor);
                if (!argument)
                    break;
                int is_flag = argument[0] == '-';
                free(argument);
                if (!is_flag) {
                    is_neutral = 1;
                    break;
                }
            }
            if (!is_neutral) {
                is_neutral = 1;
                argument_cursor = skip_whitespace(next, end);
            }
            next = argument_cursor;
        } else if (strcmp(command, "popd") == 0 || is_variable_assignment(command)) {
            is_neutral = 1;
        } else if (strcmp(command, "env") == 0) {
            const char *argument_cursor = next;
            for (;;) {
                const char *argument_start = argument_cursor;
                char *argument = take_word(argument_cursor, end, &argument_cursor);
                if (!argument)
                    break;
                int is_assignment = is_variable_assignment(argument);
                free(argument);
                if (!is_assignment) {
                    argument_cursor = argument_start;
                    break;
                }
            }
            is_neutral = 1;
            next = argument_cursor;
        }
        free(command);
        if (!is_neutral)
            return cursor;

        consumed_prefix = 1;
        cursor = skip_whitespace(next, end);
        if (cursor >= end) {
            *neutral_only_out = 1;
            return NULL;
        }
    }
}

static const char *skip_command_word(const char *body, const char *segment_end)
{
    const char *cursor = body;
    char *command = take_word(cursor, segment_end, &cursor);
    free(command);
    return cursor;
}

/* Unknown xargs option values are rejected when mistaken for the wrapped command. */
static enum command_class classify_xargs(const char *body, const char *segment_end)
{
    const char *cursor = skip_command_word(body, segment_end);
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            return COMMAND_UNKNOWN;
        int is_flag = token[0] == '-' && token[1] != '\0';
        if (!is_flag) {
            enum command_class classification = classify_command(token, NULL);
            free(token);
            return classification;
        }
        free(token);
    }
    return COMMAND_UNKNOWN;
}

static int is_mutating_search_option(const char *token)
{
    static const char *const names[] = {
        "-delete",  "-exec", "-execdir", "-ok", "-okdir", "-fprint",      "-fprint0",
        "-fprintf", "-fls",  "-x",       "-X",  "--exec", "--exec-batch",
    };
    return NAME_IN_LIST(names, token) || strncmp(token, "--exec=", 7) == 0 ||
           strncmp(token, "--exec-batch=", 13) == 0;
}

static int file_search_mutates(const char *body, const char *segment_end)
{
    const char *cursor = skip_command_word(body, segment_end);
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            return 0;
        int mutates = is_mutating_search_option(token);
        free(token);
        if (mutates)
            return 1;
    }
    return 0;
}

static int has_output_option(const char *cursor, const char *segment_end)
{
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            return 0;
        int is_output = (token[0] == '-' && token[1] == 'o' && token[2] != '-') ||
                        strcmp(token, "--output") == 0 || strncmp(token, "--output=", 9) == 0;
        free(token);
        if (is_output)
            return 1;
    }
    return 0;
}

/* Requiring a non-alphanumeric left boundary avoids matching script text, at the cost of missing
 * numeric addresses such as `1w out`. */
static int sed_script_writes(const char *script)
{
    size_t script_len = strlen(script);
    for (size_t offset = 0; offset < script_len; offset++) {
        char command = script[offset];
        if (command != 'w' && command != 'W' && command != 'e')
            continue;
        char next = script[offset + 1];
        if (next != ' ' && next != '\t' && next != '\0' && next != ';' && next != '\n' &&
            next != '}')
            continue;

        size_t command_start = offset;
        while (command_start > 0 &&
               (script[command_start - 1] == ' ' || script[command_start - 1] == '\t'))
            command_start--;
        if (command_start == 0 || (!isalnum((unsigned char)script[command_start - 1]) &&
                                   script[command_start - 1] != '-'))
            return 1;
    }
    return 0;
}

static int awk_writes(const char *cursor, const char *segment_end)
{
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            return 0;
        if (strcmp(token, "-i") == 0) {
            free(token);
            char *extension = take_word(cursor, segment_end, &cursor);
            if (!extension)
                return 0;
            int writes = strcmp(extension, "inplace") == 0;
            free(extension);
            if (writes)
                return 1;
            continue;
        }
        int writes = (token[0] == '-' && token[1] == 'i' && strcmp(token + 2, "inplace") == 0) ||
                     strstr(token, "system(") != NULL;
        free(token);
        if (writes)
            return 1;
    }
    return 0;
}

static int sed_filter_writes(const char *cursor, const char *segment_end)
{
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            return 0;
        int writes = sed_script_writes(token);
        free(token);
        if (writes)
            return 1;
    }
    return 0;
}

static int format_filter_writes(const char *body, const char *segment_end, const char *command)
{
    const char *arguments = skip_command_word(body, segment_end);
    if (strcmp(command, "sort") == 0)
        return has_output_option(arguments, segment_end);
    if (strcmp(command, "awk") == 0)
        return awk_writes(arguments, segment_end);
    if (strcmp(command, "sed") == 0)
        return sed_filter_writes(arguments, segment_end);
    return 0;
}

static int tree_writes(const char *body, const char *segment_end)
{
    return has_output_option(skip_command_word(body, segment_end), segment_end);
}

/* `more` may resolve to `less`, so both commands reject less's output options. */
static int pager_writes(const char *body, const char *segment_end)
{
    const char *cursor = skip_command_word(body, segment_end);
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            return 0;
        int writes = (token[0] == '-' && (token[1] == 'o' || token[1] == 'O') && token[2] != '-') ||
                     strcmp(token, "--log-file") == 0 || strncmp(token, "--log-file=", 11) == 0;
        free(token);
        if (writes)
            return 1;
    }
    return 0;
}

/* A dot starts the backup suffix in BSD forms such as `-Ei.bak`. */
static int sed_edits_in_place(const char *body, const char *segment_end)
{
    const char *cursor = skip_command_word(body, segment_end);
    while (cursor < segment_end) {
        char *token = take_word(cursor, segment_end, &cursor);
        if (!token)
            return 0;
        int edits = strncmp(token, "--in-place", 10) == 0;
        if (!edits && token[0] == '-' && token[1] != '-' && token[1] != '\0') {
            for (size_t i = 1; token[i] && token[i] != '.'; i++) {
                if (token[i] == 'i') {
                    edits = 1;
                    break;
                }
            }
        }
        free(token);
        if (edits)
            return 1;
    }
    return 0;
}

static int known_command_writes(const char *command, const char *body, const char *segment_end)
{
    if ((strcmp(command, "find") == 0 || strcmp(command, "fd") == 0) &&
        file_search_mutates(body, segment_end))
        return 1;
    if (strcmp(command, "tree") == 0 && tree_writes(body, segment_end))
        return 1;
    return (strcmp(command, "less") == 0 || strcmp(command, "more") == 0) &&
           pager_writes(body, segment_end);
}

static enum command_class classify_format_filter(const char *command, const char *body,
                                                 const char *segment_end)
{
    if (is_content_filter(command))
        return COMMAND_FILTER;

    const struct command_spec *spec = command_spec_for(command);
    if (!spec)
        return COMMAND_UNKNOWN;
    if (count_operands(spec, body, segment_end) < spec->required_operands)
        return COMMAND_FILTER;
    if (format_filter_writes(body, segment_end, command))
        return COMMAND_UNKNOWN;
    return COMMAND_EXPLORATION;
}

static enum command_class classify_segment(const char *segment, const char *segment_end)
{
    int neutral_only = 0;
    const char *body = skip_neutral_prefix(segment, segment_end, &neutral_only);
    if (neutral_only)
        return COMMAND_EXPLORATION;
    if (!body)
        return COMMAND_UNKNOWN;

    const char *cursor = body;
    char *command = take_word(cursor, segment_end, &cursor);
    if (!command)
        return COMMAND_UNKNOWN;
    char *subcommand = take_word(cursor, segment_end, &cursor);

    enum command_class classification = classify_command(command, subcommand);
    if (classification != COMMAND_UNKNOWN) {
        if (known_command_writes(command, body, segment_end)) {
            classification = COMMAND_UNKNOWN;
        } else {
            const struct command_spec *spec = command_spec_for(command);
            if (spec && count_operands(spec, body, segment_end) < spec->required_operands)
                classification = COMMAND_FILTER;
        }
    } else if (strcmp(command, "xargs") == 0) {
        classification = classify_xargs(body, segment_end);
    } else if (strcmp(command, "sed") == 0 && sed_edits_in_place(body, segment_end)) {
        classification = COMMAND_UNKNOWN;
    } else if (is_format_filter(command)) {
        classification = classify_format_filter(command, body, segment_end);
    }

    free(command);
    free(subcommand);
    return classification;
}

enum command_separator {
    SEPARATOR_START,
    SEPARATOR_PIPE,
    SEPARATOR_STATEMENT,
};

typedef int (*segment_callback_fn)(const char *segment, const char *segment_end,
                                   enum command_separator previous_separator, void *user_data);

/* Return 0 when the callback stops traversal early. */
static int for_each_segment(const char *command, segment_callback_fn callback, void *user_data)
{
    size_t command_len = strlen(command);
    const char *command_end = command + command_len;
    const char *segment = command;
    enum command_separator previous_separator = SEPARATOR_START;
    char quote = 0;
    for (size_t offset = 0; offset < command_len;) {
        char c = command[offset];
        if (quote) {
            /* Backslashes are literal inside POSIX single quotes. */
            if (quote == '"' && c == '\\' && command[offset + 1])
                offset += 2;
            else if (c == quote) {
                quote = 0;
                offset++;
            } else {
                offset++;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            offset++;
            continue;
        }
        if (c == '\\' && command[offset + 1]) {
            offset += 2;
            continue;
        }
        size_t separator_len = 0;
        enum command_separator separator = SEPARATOR_STATEMENT;
        if ((c == '&' && command[offset + 1] == '&') || (c == '|' && command[offset + 1] == '|')) {
            separator_len = 2;
        } else if (c == ';' || c == '\n' || c == '\r') {
            separator_len = 1;
        } else if (c == '|') {
            separator_len = 1;
            separator = SEPARATOR_PIPE;
        }
        if (separator_len) {
            if (!callback(segment, command + offset, previous_separator, user_data))
                return 0;
            previous_separator = separator;
            segment = command + offset + separator_len;
            offset += separator_len;
            continue;
        }
        offset++;
    }
    return callback(segment, command_end, previous_separator, user_data);
}

struct classifier_state {
    int has_substantive_command;
    int statement_has_producer;
};

static int classify_segment_callback(const char *segment, const char *segment_end,
                                     enum command_separator previous_separator, void *user_data)
{
    struct classifier_state *state = user_data;

    /* Reset before skipping empty segments so their preceding boundary is not lost. */
    if (previous_separator != SEPARATOR_PIPE)
        state->statement_has_producer = 0;

    const char *content = skip_whitespace(segment, segment_end);
    if (content >= segment_end)
        return 1;

    enum command_class classification = classify_segment(segment, segment_end);
    if (classification == COMMAND_UNKNOWN)
        return 0;
    if (classification == COMMAND_FILTER) {
        /* A standalone filter may block on stdin or emit unrelated content. */
        return state->statement_has_producer;
    }
    state->statement_has_producer = 1;
    state->has_substantive_command = 1;
    return 1;
}

int bash_command_is_exploration(const char *command)
{
    if (!command)
        return 0;
    const char *content = skip_whitespace(command, command + strlen(command));
    if (!*content || command_has_disqualifier(command))
        return 0;

    struct classifier_state state = {0};
    return for_each_segment(command, classify_segment_callback, &state) &&
           state.has_substantive_command;
}
