/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cli.h"
#include "harness.h"
#include "provider.h"
#include "session.h"
#include "xalloc.h"
#include "system/locale.h"
#include "text/width.h"

static void test_parse_selection_and_prompt_arguments(void)
{
    char *argv[] = {"hax",
                    "--print",
                    "--raw",
                    "--bare",
                    "--no-session",
                    "--provider=test",
                    "--model=model-a",
                    "--effort=high",
                    "--preset=review",
                    "hello",
                    "world",
                    NULL};
    int argc = (int)(sizeof(argv) / sizeof(*argv)) - 1;
    struct cli_options options;

    EXPECT(cli_parse(argc, argv, &options) == CLI_PARSE_OK);
    EXPECT(options.one_shot == 1);
    EXPECT(options.agent_options.raw == 1);
    EXPECT(options.bare == 1);
    EXPECT(options.no_session == 1);
    EXPECT_STR_EQ(options.selection.provider, "test");
    EXPECT_STR_EQ(options.selection.model, "model-a");
    EXPECT_STR_EQ(options.selection.effort, "high");
    EXPECT_STR_EQ(options.selection.preset, "review");

    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, argc, argv, stdin, 1, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "hello world");
    free(prompt);
}

static void test_parse_resume_modes(void)
{
    char *latest_argv[] = {"hax", "--continue", NULL};
    struct cli_options options;
    EXPECT(cli_parse(2, latest_argv, &options) == CLI_PARSE_OK);
    EXPECT(options.resume_mode == CLI_RESUME_LATEST);

    char *id_argv[] = {"hax", "--resume=abc123", NULL};
    EXPECT(cli_parse(2, id_argv, &options) == CLI_PARSE_OK);
    EXPECT(options.resume_mode == CLI_RESUME_SELECT);
    EXPECT_STR_EQ(options.resume_id, "abc123");

    char *picker_argv[] = {"hax", "--resume", NULL};
    EXPECT(cli_parse(2, picker_argv, &options) == CLI_PARSE_OK);
    EXPECT(options.resume_mode == CLI_RESUME_SELECT);
    EXPECT(options.resume_id == NULL);
}

static void test_parse_rejects_incompatible_resume_options(void)
{
    struct cli_options options;
    char *continue_first[] = {"hax", "--continue", "--resume=id", NULL};
    EXPECT(cli_parse(3, continue_first, &options) == CLI_PARSE_ERROR);

    char *resume_first[] = {"hax", "--resume=id", "--continue", NULL};
    EXPECT(cli_parse(3, resume_first, &options) == CLI_PARSE_ERROR);

    char *picker_oneshot[] = {"hax", "--print", "--resume", NULL};
    EXPECT(cli_parse(3, picker_oneshot, &options) == CLI_PARSE_ERROR);
}

static void test_parse_rejects_missing_values_and_interactive_prompt(void)
{
    struct cli_options options;
    char *empty_provider[] = {"hax", "--provider=", NULL};
    EXPECT(cli_parse(2, empty_provider, &options) == CLI_PARSE_ERROR);

    char *empty_resume[] = {"hax", "--resume=", NULL};
    EXPECT(cli_parse(2, empty_resume, &options) == CLI_PARSE_ERROR);

    char *interactive_prompt[] = {"hax", "hello", NULL};
    EXPECT(cli_parse(2, interactive_prompt, &options) == CLI_PARSE_ERROR);
}

static void test_parse_json_implies_print(void)
{
    struct cli_options options;
    char *json_alone[] = {"hax", "--json", "hi", NULL};
    EXPECT(cli_parse(3, json_alone, &options) == CLI_PARSE_OK);
    EXPECT(options.agent_options.json == 1);
    EXPECT(options.one_shot == 1);

    char *json_with_print[] = {"hax", "-p", "--json", "hi", NULL};
    EXPECT(cli_parse(4, json_with_print, &options) == CLI_PARSE_OK);
    EXPECT(options.agent_options.json == 1);
    EXPECT(options.one_shot == 1);

    char *plain_oneshot[] = {"hax", "-p", "hi", NULL};
    EXPECT(cli_parse(3, plain_oneshot, &options) == CLI_PARSE_OK);
    EXPECT(options.agent_options.json == 0);
}

static void test_parse_version_prints_and_exits(void)
{
    const char *flags[] = {"--version", "-v"};
    for (size_t i = 0; i < sizeof(flags) / sizeof(*flags); i++) {
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        EXPECT(saved >= 0);
        FILE *tmp = tmpfile();
        EXPECT(tmp != NULL);
        EXPECT(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

        char *argv[] = {"hax", (char *)flags[i], NULL};
        struct cli_options options;
        enum cli_parse_result result = cli_parse(2, argv, &options);

        fflush(stdout);
        EXPECT(dup2(saved, STDOUT_FILENO) >= 0);
        close(saved);
        EXPECT(result == CLI_PARSE_EXIT);

        EXPECT(fseek(tmp, 0, SEEK_SET) == 0);
        char line[256] = "";
        EXPECT(fgets(line, sizeof(line), tmp) != NULL);
        fclose(tmp);
        EXPECT(strncmp(line, "hax ", 4) == 0);
        EXPECT(strlen(line) > 5 && line[strlen(line) - 1] == '\n');
    }
}

static char *capture_help_output(void)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    EXPECT(saved >= 0);
    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    EXPECT(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

    char *argv[] = {"hax", "--help", NULL};
    struct cli_options options;
    enum cli_parse_result result = cli_parse(2, argv, &options);

    fflush(stdout);
    EXPECT(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    EXPECT(result == CLI_PARSE_EXIT);

    EXPECT(fseek(tmp, 0, SEEK_END) == 0);
    long size = ftell(tmp);
    EXPECT(size > 0);
    EXPECT(fseek(tmp, 0, SEEK_SET) == 0);
    char *out = xmalloc((size_t)size + 1);
    EXPECT(fread(out, 1, (size_t)size, tmp) == (size_t)size);
    out[size] = '\0';
    fclose(tmp);
    return out;
}

static void expect_help_rows_fit(const char *out, size_t max_cells)
{
    const char *row = out;
    while (row && *row) {
        const char *end = strchr(row, '\n');
        size_t row_bytes = end ? (size_t)(end - row) : strlen(row);
        char *copy = xmalloc(row_bytes + 1);
        memcpy(copy, row, row_bytes);
        copy[row_bytes] = '\0';
        if (display_cells(copy) > max_cells)
            FAIL("row exceeds %zu cells: %s", max_cells, copy);
        free(copy);
        row = end ? end + 1 : NULL;
    }
}

static void test_help_wraps_to_display_width(void)
{
    setenv("HAX_DISPLAY_WIDTH", "60", 1);
    char *out = capture_help_output();
    EXPECT(strstr(out, "usage:") != NULL);
    EXPECT(strstr(out, "--resume[=ID]") != NULL);
    EXPECT(strstr(out, "README.md") != NULL);
    expect_help_rows_fit(out, 60);
    free(out);

    /* Narrow widths drop the flag column and stack descriptions under their flags. */
    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    out = capture_help_output();
    unsetenv("HAX_DISPLAY_WIDTH");
    expect_help_rows_fit(out, 30);
    EXPECT(strstr(out, "barebones chat") != NULL);
    free(out);
}

static FILE *prompt_stream(const char *text)
{
    FILE *stream = tmpfile();
    EXPECT(stream != NULL);
    EXPECT(fwrite(text, 1, strlen(text), stream) == strlen(text));
    EXPECT(fseek(stream, 0, SEEK_SET) == 0);
    return stream;
}

static void test_read_prompt_from_stream(void)
{
    char *argv[] = {"hax", "--print", NULL};
    struct cli_options options;
    EXPECT(cli_parse(2, argv, &options) == CLI_PARSE_OK);

    FILE *stream = prompt_stream("first\nsecond\r\n");
    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 2, argv, stream, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "first\nsecond");
    free(prompt);
    fclose(stream);

    stream = prompt_stream("first\n\n");
    EXPECT(cli_read_prompt(&options, 2, argv, stream, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "first\n");
    free(prompt);
    fclose(stream);
}

static void test_read_prompt_rejects_missing_or_empty_input(void)
{
    char *argv[] = {"hax", "--print", NULL};
    struct cli_options options;
    EXPECT(cli_parse(2, argv, &options) == CLI_PARSE_OK);

    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 2, argv, stdin, 1, &prompt) == -1);
    EXPECT(prompt == NULL);

    FILE *stream = prompt_stream("\n");
    EXPECT(cli_read_prompt(&options, 2, argv, stream, 0, &prompt) == -1);
    EXPECT(prompt == NULL);
    fclose(stream);
}

/* A resumed one-shot run treats a missing or empty prompt as "continue the conversation". */
static void test_read_prompt_promptless_resume_continues(void)
{
    char *argv[] = {"hax", "--print", "--resume=abc123", NULL};
    struct cli_options options;
    EXPECT(cli_parse(3, argv, &options) == CLI_PARSE_OK);

    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 3, argv, stdin, 1, &prompt) == 0);
    EXPECT(prompt == NULL);

    FILE *stream = prompt_stream("\n");
    EXPECT(cli_read_prompt(&options, 3, argv, stream, 0, &prompt) == 0);
    EXPECT(prompt == NULL);
    fclose(stream);
}

static void test_resolve_missing_session(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    setenv("HAX_SESSION_RETENTION_DAYS", "0", 1);

    struct cli_options options = {.resume_mode = CLI_RESUME_LATEST, .one_shot = 1};
    char *resolved = NULL;
    EXPECT(cli_resolve_session(&options, &resolved) == CLI_SESSION_ERROR);
    EXPECT(resolved == NULL);

    options.one_shot = 0;
    EXPECT(cli_resolve_session(&options, &resolved) == CLI_SESSION_READY);
    EXPECT(resolved == NULL);

    options.resume_mode = CLI_RESUME_SELECT;
    options.resume_id = "missing";
    EXPECT(cli_resolve_session(&options, &resolved) == CLI_SESSION_ERROR);
    EXPECT(resolved == NULL);
}

static char *create_session(char **id)
{
    *id = NULL;
    struct session_log *log = session_log_open("test", "model", NULL, NULL, NULL);
    EXPECT(log != NULL);
    if (!log)
        return NULL;

    char *path = xstrdup(session_log_path(log));
    struct item prompt = {.kind = ITEM_USER_MESSAGE, .text = (char *)"prompt"};
    session_log_append(log, &prompt, 1);
    session_log_close(log);

    struct session_meta metadata = {0};
    EXPECT(session_read_meta(path, &metadata) == 0);
    if (metadata.id)
        *id = xstrdup(metadata.id);
    session_meta_free(&metadata);
    return path;
}

static void test_resolve_session_by_latest_id_and_prefix(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
    setenv("HAX_SESSION_RETENTION_DAYS", "0", 1);

    char *first_id;
    char *first_path = create_session(&first_id);
    char *second_id;
    char *second_path = create_session(&second_id);
    if (!first_path || !first_id || !second_path || !second_id) {
        free(first_id);
        free(first_path);
        free(second_id);
        free(second_path);
        return;
    }

    /* Back-date the first session rather than trusting two rapid writes to land on distinct
     * timestamps: OpenBSD stamps both with the same mtime, which leaves "latest" undefined and
     * tests the filesystem's clock resolution instead of the resolution logic. */
    struct timespec stamps[2] = {{.tv_nsec = UTIME_OMIT}, {.tv_sec = time(NULL) - 60}};
    EXPECT(utimensat(AT_FDCWD, first_path, stamps, 0) == 0);

    struct cli_options options = {.resume_mode = CLI_RESUME_LATEST, .one_shot = 1};
    char *resolved = NULL;
    EXPECT(cli_resolve_session(&options, &resolved) == CLI_SESSION_READY);
    EXPECT_STR_EQ(resolved, second_path);
    free(resolved);

    options.resume_mode = CLI_RESUME_SELECT;
    options.resume_id = first_id;
    EXPECT(cli_resolve_session(&options, &resolved) == CLI_SESSION_READY);
    EXPECT_STR_EQ(resolved, first_path);
    free(resolved);

    char prefix[9];
    memcpy(prefix, second_id, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
    options.resume_id = prefix;
    EXPECT(cli_resolve_session(&options, &resolved) == CLI_SESSION_READY);
    EXPECT_STR_EQ(resolved, second_path);
    free(resolved);

    free(first_id);
    free(first_path);
    free(second_id);
    free(second_path);
}

static void test_subagent_depth_validation(void)
{
    const char *original = getenv("HAX_SUBAGENT_DEPTH");
    char *saved = original ? strdup(original) : NULL;

    unsetenv("HAX_SUBAGENT_DEPTH");
    EXPECT(cli_check_subagent_depth() == 0);
    setenv("HAX_SUBAGENT_DEPTH", "2", 1);
    EXPECT(cli_check_subagent_depth() == 0);
    setenv("HAX_SUBAGENT_DEPTH", "3", 1);
    EXPECT(cli_check_subagent_depth() == -1);
    setenv("HAX_SUBAGENT_DEPTH", "invalid", 1);
    EXPECT(cli_check_subagent_depth() == -1);
    setenv("HAX_SUBAGENT_DEPTH", "-1", 1);
    EXPECT(cli_check_subagent_depth() == -1);

    if (saved)
        setenv("HAX_SUBAGENT_DEPTH", saved, 1);
    else
        unsetenv("HAX_SUBAGENT_DEPTH");
    free(saved);
}

int main(void)
{
    locale_init_utf8();
    test_parse_selection_and_prompt_arguments();
    test_parse_resume_modes();
    test_parse_rejects_incompatible_resume_options();
    test_parse_rejects_missing_values_and_interactive_prompt();
    test_parse_json_implies_print();
    test_parse_version_prints_and_exits();
    test_help_wraps_to_display_width();
    test_read_prompt_from_stream();
    test_read_prompt_rejects_missing_or_empty_input();
    test_read_prompt_promptless_resume_continues();
    test_resolve_missing_session();
    test_resolve_session_by_latest_id_and_prefix();
    test_subagent_depth_validation();
    T_REPORT();
}
