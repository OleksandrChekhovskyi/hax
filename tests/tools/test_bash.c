/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "util.h"

static char *call_bash(const char *cmd_json_escaped)
{
    char *args = xasprintf("{\"command\":\"%s\"}", cmd_json_escaped);
    char *out = TOOL_BASH.run(args);
    free(args);
    return out;
}

static void test_bash_invalid_json(void)
{
    char *out = TOOL_BASH.run("not json");
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_bash_missing_command(void)
{
    char *out = TOOL_BASH.run("{}");
    EXPECT(strstr(out, "missing 'command'") != NULL);
    free(out);
}

static void test_bash_stdout(void)
{
    char *out = call_bash("echo hello");
    EXPECT_STR_EQ(out, "hello\n");
    free(out);
}

static void test_bash_stderr_merged(void)
{
    char *out = call_bash("echo err 1>&2");
    EXPECT_STR_EQ(out, "err\n");
    free(out);
}

static void test_bash_exit_code(void)
{
    char *out = call_bash("false");
    EXPECT(strstr(out, "[exit 1]") != NULL);
    free(out);
}

static void test_bash_signal(void)
{
    /* Self-kill with SIGTERM. */
    char *out = call_bash("kill -TERM $$");
    EXPECT(strstr(out, "[signal 15]") != NULL);
    free(out);
}

static void test_bash_no_output(void)
{
    char *out = call_bash("true");
    EXPECT_STR_EQ(out, "(no output)");
    free(out);
}

static void test_bash_stdin_detached(void)
{
    /* `cat` with no args reads stdin; if stdin wasn't /dev/null'd, it would
     * block forever. With EOF on stdin it returns immediately and produces
     * no output. */
    time_t t0 = time(NULL);
    char *out = call_bash("cat");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 3);
    EXPECT_STR_EQ(out, "(no output)");
    free(out);
}

static void test_bash_background_job_does_not_hang(void)
{
    /* Shell exits immediately; backgrounded sleep would keep the pipe open
     * unless pgroup cleanup releases it. */
    time_t t0 = time(NULL);
    char *out = call_bash("echo spawned; sleep 30 &");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 3);
    EXPECT(strstr(out, "spawned") != NULL);
    free(out);
}

static void test_bash_foreground_infinite_writer_caps(void)
{
    /* `yes` never exits on its own; must be killed once OUTPUT_CAP is hit. */
    time_t t0 = time(NULL);
    char *out = call_bash("yes foo");
    time_t elapsed = time(NULL) - t0;
    EXPECT(elapsed < 3);
    EXPECT(strstr(out, "[output truncated]") != NULL);
    /* We SIGKILL the pgroup, so the shell dies by signal. */
    EXPECT(strstr(out, "[signal 9]") != NULL);
    free(out);
}

static void test_bash_sanitizes_non_utf8(void)
{
    /* printf \xff produces an invalid UTF-8 byte which must be replaced.
     * Quadruple-backslash: C-literal → JSON → shell each eat one layer. */
    char *out = call_bash("printf '\\\\xff'");
    EXPECT(strstr(out, "\xEF\xBF\xBD") != NULL);
    free(out);
}

int main(void)
{
    test_bash_invalid_json();
    test_bash_missing_command();
    test_bash_stdout();
    test_bash_stderr_merged();
    test_bash_exit_code();
    test_bash_signal();
    test_bash_no_output();
    test_bash_stdin_detached();
    test_bash_background_job_does_not_hang();
    test_bash_foreground_infinite_writer_caps();
    test_bash_sanitizes_non_utf8();
    T_REPORT();
}
