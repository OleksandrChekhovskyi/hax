/* SPDX-License-Identifier: MIT */
#include "file_mention.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* POSIX declares the wait macros here; some libc headers also expose them indirectly. */
#include <sys/wait.h> // IWYU pragma: keep

#include "diag.h"
#include "xalloc.h"
#include "system/fs.h"
#include "system/path.h"
#include "system/spawn.h"
#include "terminal/input_core.h"
#include "text/shell_quote.h"

/* NUL records preserve non-ASCII paths that git line mode would quote and filenames containing
 * newlines. Keep candidates streaming: fzf reads stdin asynchronously while using /dev/tty. */
#define FILE_CANDIDATES_COMMAND                                                                    \
    "git ls-files -z --cached --others --exclude-standard 2>/dev/null"                             \
    " || find . \\( -name .git -o -name node_modules \\) -prune -o -type f -print0 2>/dev/null"

/* `%%` is a literal percent in the xasprintf format. */
#define FZF_COMMAND_SUFFIX                                                                         \
    " | fzf --read0 --print0 --height=~40%% --layout=reverse --scheme=path --query=%s"

/* Includes the terminating NUL; longer selections are rejected rather than truncated. */
#define FZF_SELECTION_CAPACITY 4096

struct picker_query {
    char *root;
    char *filter;
};

enum record_result {
    RECORD_END,
    RECORD_COMPLETE,
    RECORD_TOO_LONG,
};

static int fzf_available(void)
{
    char *path = fs_which("fzf");

    if (!path)
        return 0;
    free(path);
    return 1;
}

static int query_uses_external_root(const char *query)
{
    if (!query || !*query)
        return 0;
    if (query[0] == '/')
        return 1;
    if (query[0] == '~' && (query[1] == '\0' || query[1] == '/'))
        return 1;
    return query[0] == '.' && query[1] == '.' && (query[2] == '\0' || query[2] == '/');
}

static struct picker_query parse_picker_query(const char *text)
{
    struct picker_query query = {0};

    if (!text)
        text = "";
    if (!query_uses_external_root(text)) {
        query.filter = xstrdup(text);
        return query;
    }

    const char *slash = strrchr(text, '/');
    if (!slash) {
        query.root = xasprintf("%s/", text);
        query.filter = xstrdup("");
        return query;
    }

    size_t root_len = (size_t)(slash - text + 1);
    query.root = xmalloc(root_len + 1);
    memcpy(query.root, text, root_len);
    query.root[root_len] = '\0';
    query.filter = xstrdup(slash + 1);
    return query;
}

static char *build_fzf_command(const struct picker_query *query)
{
    char *quoted_filter = shell_single_quote(query->filter);
    char *command;

    if (query->root) {
        /* Shell quoting suppresses tilde expansion, so expand it before quoting. */
        char *expanded_root = path_expand_home(query->root);
        char *quoted_root = shell_single_quote(expanded_root);

        command = xasprintf("{ cd %s 2>/dev/null && { " FILE_CANDIDATES_COMMAND
                            "; }; }" FZF_COMMAND_SUFFIX,
                            quoted_root, quoted_filter);
        free(quoted_root);
        free(expanded_root);
    } else {
        command = xasprintf("{ " FILE_CANDIDATES_COMMAND "; }" FZF_COMMAND_SUFFIX, quoted_filter);
    }

    free(quoted_filter);
    return command;
}

char *file_mention_build_fzf_command(const char *query_text)
{
    struct picker_query query = parse_picker_query(query_text);
    char *command = build_fzf_command(&query);

    free(query.root);
    free(query.filter);
    return command;
}

/* Accept EOF after bytes as a complete record; fzf variants may omit the trailing NUL. */
static enum record_result read_record(FILE *stream, char *record, size_t capacity)
{
    size_t len = 0;
    int too_long = 0;

    for (;;) {
        int byte = fgetc(stream);

        if (byte == EOF && len == 0 && !too_long)
            return RECORD_END;
        if (byte == EOF || byte == '\0') {
            if (too_long)
                return RECORD_TOO_LONG;
            record[len] = '\0';
            return RECORD_COMPLETE;
        }
        if (len + 1 < capacity)
            record[len++] = (char)byte;
        else
            too_long = 1;
    }
}

static char *pick_with_fzf(const char *query_text)
{
    struct picker_query query = parse_picker_query(query_text);
    /* Paths are bytes hax renders and reads back, and fzf draws them in a full-screen UI of its
     * own, so a non-ASCII name has to survive the round trip intact. */
    char *command = spawn_shell_cmd_force_utf8(build_fzf_command(&query));
    char *picked_path = NULL;
    struct spawn_pipe pipe;

    if (spawn_pipe_open_read(&pipe, command) == 0) {
        char record[FZF_SELECTION_CAPACITY];

        if (read_record(pipe.stream, record, sizeof(record)) == RECORD_COMPLETE) {
            const char *relative_path = strncmp(record, "./", 2) == 0 ? record + 2 : record;

            if (*relative_path)
                picked_path =
                    query.root ? path_join(query.root, relative_path) : xstrdup(relative_path);
        }

        int status = spawn_pipe_close(&pipe);
        if (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            free(picked_path);
            picked_path = NULL;
        }
    }

    free(command);
    free(query.root);
    free(query.filter);
    return picked_path;
}

int file_mention_available(void)
{
    return fzf_available();
}

char *file_mention_pick(const char *query_text)
{
    if (!fzf_available()) {
        hax_warn("@file completion needs fzf installed");
        return NULL;
    }

    char *path = pick_with_fzf(query_text);
    if (!path)
        return NULL;

    /* The selection may refer to a stale git entry or a file deleted while fzf was open. */
    char *expanded_path = path_expand_home(path);
    if (fs_check_regular(expanded_path) != 0) {
        hax_warn("cannot mention '%s': %s", path, strerror(errno));
        free(expanded_path);
        free(path);
        return NULL;
    }

    free(expanded_path);
    return path;
}

static int match_file_mention(const char *buffer, size_t buffer_len, size_t cursor, size_t *start,
                              size_t *end, void *user)
{
    (void)user;

    if (cursor == 0 || cursor > buffer_len)
        return 0;

    size_t token_start = cursor;
    while (token_start > 0 && !isspace((unsigned char)buffer[token_start - 1]))
        token_start--;
    if (token_start >= cursor || buffer[token_start] != '@')
        return 0;

    *start = token_start;
    *end = cursor;
    return 1;
}

static char *pick_file_mention(const char *mention, void *user)
{
    (void)user;
    return file_mention_pick(mention + 1);
}

const struct input_modal_completer file_mention_completer = {
    .match = match_file_mention,
    .pick = pick_file_mention,
};
