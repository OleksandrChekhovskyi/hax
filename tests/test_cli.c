/* SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "cli.h"
#include "config.h"
#include "harness.h"
#include "late_writer.h"
#include "output.h"
#include "provider.h"
#include "session.h"
#include "xalloc.h"
#include "system/locale.h"

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
    EXPECT(cli_read_prompt(&options, argc, argv, STDIN_FILENO, 1, 0, &prompt) == 0);
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

static void test_parse_leading_preset(void)
{
    EXPECT(config_load("{\"presets\": {\"review\": {\"provider\": \"test\"}, "
                       "\"daily\": {\"provider\": \"test\"}}}") == 0);
    struct cli_options options;

    char *interactive[] = {"hax", "review", NULL};
    EXPECT(cli_parse(2, interactive, &options) == CLI_PARSE_OK);
    EXPECT_STR_EQ(options.selection.preset, "review");
    EXPECT(options.one_shot == 0);

    char *with_flags[] = {"hax", "review", "-c", "--model=m", NULL};
    EXPECT(cli_parse(4, with_flags, &options) == CLI_PARSE_OK);
    EXPECT_STR_EQ(options.selection.preset, "review");
    EXPECT_STR_EQ(options.selection.model, "m");
    EXPECT(options.resume_mode == CLI_RESUME_LATEST);

    char *oneshot[] = {"hax", "review", "-p", "fix", "the", "tests", NULL};
    EXPECT(cli_parse(6, oneshot, &options) == CLI_PARSE_OK);
    EXPECT_STR_EQ(options.selection.preset, "review");
    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 6, oneshot, STDIN_FILENO, 1, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "fix the tests");
    free(prompt);

    /* Only the word right after the program name is a preset; later ones are prompt text. */
    char *after_print[] = {"hax", "-p", "review", "fix", NULL};
    EXPECT(cli_parse(4, after_print, &options) == CLI_PARSE_OK);
    EXPECT(options.selection.preset == NULL);
    EXPECT(cli_read_prompt(&options, 4, after_print, STDIN_FILENO, 1, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "review fix");
    free(prompt);

    char *after_flag[] = {"hax", "-c", "review", NULL};
    EXPECT(cli_parse(3, after_flag, &options) == CLI_PARSE_ERROR);

    char *both_forms[] = {"hax", "review", "--preset=daily", NULL};
    EXPECT(cli_parse(3, both_forms, &options) == CLI_PARSE_ERROR);

    char *unknown[] = {"hax", "reviw", NULL};
    EXPECT(cli_parse(2, unknown, &options) == CLI_PARSE_ERROR);

    /* A first word that is not a preset keeps its prompt meaning. */
    char *trailing_print[] = {"hax", "fix", "the", "tests", "-p", NULL};
    EXPECT(cli_parse(5, trailing_print, &options) == CLI_PARSE_OK);
    EXPECT(options.selection.preset == NULL);
    EXPECT(cli_read_prompt(&options, 5, trailing_print, STDIN_FILENO, 1, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "fix the tests");
    free(prompt);

    EXPECT(config_load(NULL) == 0);
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

struct flag_call {
    const char *flag;
    enum cli_parse_result result;
};

static void parse_flag(void *data)
{
    struct flag_call *call = data;
    char *argv[] = {"hax", (char *)call->flag, NULL};
    struct cli_options options;
    call->result = cli_parse(2, argv, &options);
}

/* Return what parsing `flag` alone printed; the flag must exit after printing. */
static char *capture_flag_output(const char *flag)
{
    struct flag_call call = {.flag = flag};
    char *out = t_capture_stdout(parse_flag, &call);
    EXPECT(call.result == CLI_PARSE_EXIT);
    return out;
}

static void test_parse_version_prints_and_exits(void)
{
    const char *flags[] = {"--version", "-v"};
    for (size_t i = 0; i < sizeof(flags) / sizeof(*flags); i++) {
        char *out = capture_flag_output(flags[i]);
        const char *newline = strchr(out, '\n');
        EXPECT(strncmp(out, "hax ", 4) == 0);
        EXPECT(newline && newline - out > 4);
        free(out);
    }
}

static void test_help_wraps_to_display_width(void)
{
    setenv("HAX_DISPLAY_WIDTH", "60", 1);
    char *out = capture_flag_output("--help");
    EXPECT(strstr(out, "usage:") != NULL);
    EXPECT(strstr(out, "--resume[=ID]") != NULL);
    EXPECT(strstr(out, "hax-config(5)") != NULL);
    t_expect_rows_fit(out, 60);
    free(out);

    /* Narrow widths drop the flag column and stack descriptions under their flags. */
    setenv("HAX_DISPLAY_WIDTH", "30", 1);
    out = capture_flag_output("--help");
    unsetenv("HAX_DISPLAY_WIDTH");
    t_expect_rows_fit(out, 30);
    EXPECT(strstr(out, "barebones chat") != NULL);
    free(out);
}

/* Return a descriptor reading `text` from an unlinked temporary file. */
static int prompt_input(const char *text)
{
    FILE *stream = tmpfile();
    EXPECT(stream != NULL);
    EXPECT(fwrite(text, 1, strlen(text), stream) == strlen(text));
    EXPECT(fflush(stream) == 0);
    int input = dup(fileno(stream));
    fclose(stream);
    EXPECT(input >= 0 && lseek(input, 0, SEEK_SET) == 0);
    return input;
}

static void test_read_prompt_from_input(void)
{
    char *argv[] = {"hax", "--print", NULL};
    struct cli_options options;
    EXPECT(cli_parse(2, argv, &options) == CLI_PARSE_OK);

    int input = prompt_input("first\nsecond\r\n");
    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 2, argv, input, 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "first\nsecond");
    free(prompt);
    close(input);

    input = prompt_input("\nfirst\n\n");
    EXPECT(cli_read_prompt(&options, 2, argv, input, 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "first");
    free(prompt);
    close(input);
}

static void test_read_prompt_appends_input_to_arguments(void)
{
    char *argv[] = {"hax", "--print", "review", "this", NULL};
    struct cli_options options;
    EXPECT(cli_parse(4, argv, &options) == CLI_PARSE_OK);

    int input = prompt_input("\n  indented\nline\n\n");
    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 4, argv, input, 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "review this\n\n<stdin>\n  indented\nline\n</stdin>");
    free(prompt);
    close(input);

    input = prompt_input(" \n");
    EXPECT(cli_read_prompt(&options, 4, argv, input, 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "review this");
    free(prompt);
    close(input);

    char *blank_argv[] = {"hax", "--print", " ", NULL};
    EXPECT(cli_parse(3, blank_argv, &options) == CLI_PARSE_OK);
    input = prompt_input("piped alone");
    EXPECT(cli_read_prompt(&options, 3, blank_argv, input, 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "piped alone");
    free(prompt);
    close(input);
}

/* A shell pipe is waited for past the grace period, however late its producer writes. */
static void test_read_prompt_waits_for_slow_pipe(void)
{
    char *argv[] = {"hax", "--print", "summarize", NULL};
    struct cli_options options;
    EXPECT(cli_parse(3, argv, &options) == CLI_PARSE_OK);

    pid_t writer;
    int input = late_writer_pipe("", "late", &writer);
    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 3, argv, input, 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "summarize\n\n<stdin>\nlate\n</stdin>");
    free(prompt);
    close(input);
    late_writer_wait(writer);
}

/* A socket left open by a spawning parent must not hold a run that already has its prompt. */
static void test_read_prompt_skips_idle_socket(void)
{
    char *argv[] = {"hax", "--print", "summarize", NULL};
    struct cli_options options;
    EXPECT(cli_parse(3, argv, &options) == CLI_PARSE_OK);

    int idle[2];
    EXPECT(socketpair(AF_UNIX, SOCK_STREAM, 0, idle) == 0);
    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 3, argv, idle[0], 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "summarize");
    free(prompt);
    close(idle[0]);
    close(idle[1]);

    int ready[2];
    EXPECT(socketpair(AF_UNIX, SOCK_STREAM, 0, ready) == 0);
    EXPECT(write(ready[1], "context", 7) == 7);
    close(ready[1]);
    EXPECT(cli_read_prompt(&options, 3, argv, ready[0], 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "summarize\n\n<stdin>\ncontext\n</stdin>");
    free(prompt);
    close(ready[0]);
}

/* A write-only stdin cannot carry input, so it must not fail a run that has its prompt. */
static void test_read_prompt_skips_write_only_input(void)
{
    char *argv[] = {"hax", "--print", "summarize", NULL};
    struct cli_options options;
    EXPECT(cli_parse(3, argv, &options) == CLI_PARSE_OK);

    int input = open("/dev/null", O_WRONLY);
    EXPECT(input >= 0);
    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 3, argv, input, 0, 0, &prompt) == 0);
    EXPECT_STR_EQ(prompt, "summarize");
    free(prompt);
    close(input);
}

static void test_read_prompt_rejects_missing_or_empty_input(void)
{
    char *argv[] = {"hax", "--print", NULL};
    struct cli_options options;
    EXPECT(cli_parse(2, argv, &options) == CLI_PARSE_OK);

    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 2, argv, STDIN_FILENO, 1, 0, &prompt) == -1);
    EXPECT(prompt == NULL);

    int input = prompt_input("\n");
    EXPECT(cli_read_prompt(&options, 2, argv, input, 0, 0, &prompt) == -1);
    EXPECT(prompt == NULL);
    close(input);

    char *blank_argv[] = {"hax", "--print", " ", "\n", NULL};
    EXPECT(cli_parse(4, blank_argv, &options) == CLI_PARSE_OK);
    EXPECT(cli_read_prompt(&options, 4, blank_argv, STDIN_FILENO, 1, 0, &prompt) == -1);
    EXPECT(prompt == NULL);
}

/* A resumed one-shot run treats a missing or empty prompt as "continue the conversation". */
static void test_read_prompt_promptless_resume_continues(void)
{
    char *argv[] = {"hax", "--print", "--resume=abc123", NULL};
    struct cli_options options;
    EXPECT(cli_parse(3, argv, &options) == CLI_PARSE_OK);

    char *prompt = NULL;
    EXPECT(cli_read_prompt(&options, 3, argv, STDIN_FILENO, 1, 0, &prompt) == 0);
    EXPECT(prompt == NULL);

    int input = prompt_input("\n");
    EXPECT(cli_read_prompt(&options, 3, argv, input, 0, 0, &prompt) == 0);
    EXPECT(prompt == NULL);
    close(input);
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
    /* Outside any repository, so recorded sessions neither run git nor depend on the checkout. */
    EXPECT(chdir(t_tempdir()) == 0);
    locale_init_utf8();
    test_parse_selection_and_prompt_arguments();
    test_parse_resume_modes();
    test_parse_rejects_incompatible_resume_options();
    test_parse_rejects_missing_values_and_interactive_prompt();
    test_parse_leading_preset();
    test_parse_json_implies_print();
    test_parse_version_prints_and_exits();
    test_help_wraps_to_display_width();
    test_read_prompt_from_input();
    test_read_prompt_appends_input_to_arguments();
    test_read_prompt_waits_for_slow_pipe();
    test_read_prompt_skips_idle_socket();
    test_read_prompt_skips_write_only_input();
    test_read_prompt_rejects_missing_or_empty_input();
    test_read_prompt_promptless_resume_continues();
    test_resolve_missing_session();
    test_resolve_session_by_latest_id_and_prefix();
    test_subagent_depth_validation();
    T_REPORT();
}
