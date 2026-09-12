/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "harness.h"
#include "tool.h"
#include "terminal/question.h"
#include "tools/ask.h"

static char *questions_json(int n_questions)
{
    struct buf out;
    buf_init(&out);
    buf_append_str(&out, "{\"questions\":[");
    for (int i = 0; i < n_questions; i++) {
        if (i)
            buf_append_str(&out, ",");
        buf_append_str(&out, "{\"prompt\":\"p\",\"options\":[{\"label\":\"a\"}]}");
    }
    buf_append_str(&out, "]}");
    return buf_steal(&out);
}

static char *options_json(int n_options)
{
    struct buf out;
    buf_init(&out);
    buf_append_str(&out, "{\"questions\":[{\"prompt\":\"p\",\"options\":[");
    for (int i = 0; i < n_options; i++) {
        if (i)
            buf_append_str(&out, ",");
        buf_append_str(&out, "{\"label\":\"a\"}");
    }
    buf_append_str(&out, "]}]}");
    return buf_steal(&out);
}

static void expect_run_error(const char *args_json, const char *needle)
{
    char *out = TOOL_ASK.run(args_json, NULL);
    EXPECT(strstr(out, needle) != NULL);
    free(out);
}

static void test_ask_invalid_json(void)
{
    expect_run_error("not json", "invalid arguments");
}

static void test_ask_missing_questions(void)
{
    expect_run_error("{}", "missing 'questions' argument");
}

static void test_ask_questions_not_array(void)
{
    expect_run_error("{\"questions\":{}}", "'questions' must be an array");
}

static void test_ask_questions_empty(void)
{
    expect_run_error("{\"questions\":[]}", "'questions' must contain at least one question");
}

static void test_ask_too_many_questions(void)
{
    char *args = questions_json(6);
    expect_run_error(args, "too many questions (max 5)");
    free(args);
}

static void test_ask_question_not_object(void)
{
    expect_run_error("{\"questions\":[1]}", "question 1 must be an object");
}

static void test_ask_question_missing_prompt(void)
{
    expect_run_error("{\"questions\":[{\"options\":[{\"label\":\"a\"}]}]}",
                     "question 1 is missing the 'prompt' argument");
}

static void test_ask_question_missing_options(void)
{
    expect_run_error("{\"questions\":[{\"prompt\":\"p\"}]}",
                     "question 1 is missing the 'options' argument");
}

static void test_ask_question_empty_options(void)
{
    expect_run_error("{\"questions\":[{\"prompt\":\"p\",\"options\":[]}]}",
                     "question 1 must have at least one option");
}

static void test_ask_question_too_many_options(void)
{
    char *args = options_json(10);
    expect_run_error(args, "question 1 has too many options (max 9)");
    free(args);
}

static void test_ask_option_not_object(void)
{
    expect_run_error("{\"questions\":[{\"prompt\":\"p\",\"options\":[1]}]}",
                     "question 1 option 1 must be an object");
}

static void test_ask_option_missing_label(void)
{
    expect_run_error("{\"questions\":[{\"prompt\":\"p\",\"options\":[{\"value\":\"v\"}]}]}",
                     "question 1 option 1 is missing the 'label' argument");
}

static void test_ask_well_formed_non_tty(void)
{
    /* The harness has no tty, so a valid call must return the recoverable message, not block. */
    char *out =
        TOOL_ASK.run("{\"questions\":[{\"prompt\":\"p\",\"options\":[{\"label\":\"a\"}]}]}", NULL);
    EXPECT(strstr(out, "not interactive") != NULL);
    free(out);
}

static void test_ask_format_selected_value_equals_label(void)
{
    struct ask_answer answer = {.index = 1, .text = "Foo", .value = "Foo", .was_custom = 0};
    char *line = ask_format_answer_line("Q1", &answer);
    EXPECT_STR_EQ(line, "Q1: user selected: 1. Foo");
    free(line);
}

static void test_ask_format_selected_value_differs(void)
{
    struct ask_answer answer = {.index = 2, .text = "Foo", .value = "bar", .was_custom = 0};
    char *line = ask_format_answer_line("Q1", &answer);
    EXPECT_STR_EQ(line, "Q1: user selected: 2. Foo (value: bar)");
    free(line);
}

static void test_ask_format_free_text(void)
{
    struct ask_answer answer = {
        .index = -1, .text = "some text", .value = "some text", .was_custom = 1};
    char *line = ask_format_answer_line("Q2", &answer);
    EXPECT_STR_EQ(line, "Q2: user wrote: some text");
    free(line);
}

int main(void)
{
    if (!freopen("/dev/null", "r", stdin)) {
        perror("freopen");
        return 1;
    }

    test_ask_invalid_json();
    test_ask_missing_questions();
    test_ask_questions_not_array();
    test_ask_questions_empty();
    test_ask_too_many_questions();
    test_ask_question_not_object();
    test_ask_question_missing_prompt();
    test_ask_question_missing_options();
    test_ask_question_empty_options();
    test_ask_question_too_many_options();
    test_ask_option_not_object();
    test_ask_option_missing_label();
    test_ask_well_formed_non_tty();
    test_ask_format_selected_value_equals_label();
    test_ask_format_selected_value_differs();
    test_ask_format_free_text();
    T_REPORT();
}
