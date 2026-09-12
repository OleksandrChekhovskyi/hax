/* SPDX-License-Identifier: MIT */
#include "tools/ask.h"

#include <jansson.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "terminal/interrupt.h"
#include "terminal/question.h"

#define ASK_MAX_QUESTIONS 5
#define ASK_MAX_OPTIONS   9

#define ASK_NON_INTERACTIVE                                                                        \
    "This run's terminal is not interactive, so the question cannot be asked; answer "             \
    "differently or proceed with your best default."

#define ASK_CANCELLED "User cancelled the question"

struct ask_question_spec {
    const char *id;
    const char *label;
    char *owned_name; /* the "Q<n>" default, shared by id and label when both default */
    const char *prompt;
    struct ask_question_option options[ASK_MAX_OPTIONS];
    size_t n_options;
    int allow_other;
};

/* Optional string key: absent yields NULL, present-but-not-a-string is an error. */
static int get_optional_string(json_t *object, const char *key, const char **out)
{
    json_t *value = json_object_get(object, key);
    if (!value) {
        *out = NULL;
        return 0;
    }
    if (!json_is_string(value))
        return -1;
    *out = json_string_value(value);
    return 0;
}

static void free_specs(struct ask_question_spec *specs)
{
    for (size_t i = 0; i < ASK_MAX_QUESTIONS; i++)
        free(specs[i].owned_name);
}

/* Fill `specs` from the parsed arguments. Returns an allocated error message, or NULL on
 * success. Normalized strings borrow `root`, which must outlive the widget run. */
static char *normalize_questions(json_t *root, struct ask_question_spec *specs, size_t *n_out)
{
    json_t *questions = json_object_get(root, "questions");
    if (!questions)
        return xstrdup("missing 'questions' argument");
    if (!json_is_array(questions))
        return xstrdup("'questions' must be an array");

    size_t n_questions = json_array_size(questions);
    if (n_questions == 0)
        return xstrdup("'questions' must contain at least one question");
    if (n_questions > ASK_MAX_QUESTIONS)
        return xasprintf("too many questions (max %d)", ASK_MAX_QUESTIONS);

    for (size_t i = 0; i < n_questions; i++) {
        json_t *question = json_array_get(questions, i);
        size_t number = i + 1;
        if (!json_is_object(question))
            return xasprintf("question %zu must be an object", number);

        json_t *prompt = json_object_get(question, "prompt");
        if (!json_is_string(prompt) || !*json_string_value(prompt))
            return xasprintf("question %zu is missing the 'prompt' argument", number);
        specs[i].prompt = json_string_value(prompt);

        json_t *options = json_object_get(question, "options");
        if (!options)
            return xasprintf("question %zu is missing the 'options' argument", number);
        if (!json_is_array(options))
            return xasprintf("question %zu: 'options' must be an array", number);
        size_t n_options = json_array_size(options);
        if (n_options == 0)
            return xasprintf("question %zu must have at least one option", number);
        if (n_options > ASK_MAX_OPTIONS)
            return xasprintf("question %zu has too many options (max %d)", number, ASK_MAX_OPTIONS);

        for (size_t j = 0; j < n_options; j++) {
            json_t *option = json_array_get(options, j);
            size_t option_number = j + 1;
            if (!json_is_object(option))
                return xasprintf("question %zu option %zu must be an object", number,
                                 option_number);

            json_t *label = json_object_get(option, "label");
            if (!json_is_string(label) || !*json_string_value(label))
                return xasprintf("question %zu option %zu is missing the 'label' argument", number,
                                 option_number);

            const char *value = NULL;
            if (get_optional_string(option, "value", &value) < 0)
                return xasprintf("question %zu option %zu: 'value' must be a string", number,
                                 option_number);
            const char *description = NULL;
            if (get_optional_string(option, "description", &description) < 0)
                return xasprintf("question %zu option %zu: 'description' must be a string", number,
                                 option_number);

            const char *label_text = json_string_value(label);
            specs[i].options[j] = (struct ask_question_option){
                .label = label_text,
                .value = (value && *value) ? value : label_text,
                .description = description,
            };
        }
        specs[i].n_options = n_options;

        const char *id = NULL;
        if (get_optional_string(question, "id", &id) < 0)
            return xasprintf("question %zu: 'id' must be a string", number);
        const char *label = NULL;
        if (get_optional_string(question, "label", &label) < 0)
            return xasprintf("question %zu: 'label' must be a string", number);

        if (!id || !*id || !label || !*label) {
            specs[i].owned_name = xasprintf("Q%zu", number);
            if (!id || !*id)
                id = specs[i].owned_name;
            if (!label || !*label)
                label = specs[i].owned_name;
        }
        specs[i].id = id;
        specs[i].label = label;

        json_t *allow_other = json_object_get(question, "allowOther");
        if (allow_other && !json_is_boolean(allow_other))
            return xasprintf("question %zu: 'allowOther' must be a boolean", number);
        specs[i].allow_other = allow_other ? json_boolean_value(allow_other) : 1;
    }

    *n_out = n_questions;
    return NULL;
}

char *ask_format_answer_line(const char *label, const struct ask_answer *answer)
{
    const char *name = label ? label : "";
    const char *text = answer->text ? answer->text : "";
    if (answer->was_custom)
        return xasprintf("%s: user wrote: %s", name, text);
    if (answer->value && strcmp(answer->value, text) != 0)
        return xasprintf("%s: user selected: %d. %s (value: %s)", name, answer->index, text,
                         answer->value);
    return xasprintf("%s: user selected: %d. %s", name, answer->index, text);
}

static const char *answer_label(const struct ask_question_spec *specs, size_t n_questions,
                                const struct ask_answer *answer, size_t position)
{
    if (answer->question_id) {
        for (size_t i = 0; i < n_questions; i++)
            if (specs[i].id && strcmp(specs[i].id, answer->question_id) == 0)
                return specs[i].label;
    }
    if (position < n_questions)
        return specs[position].label;
    return answer->question_id ? answer->question_id : "";
}

static char *format_answers(const struct ask_question_spec *specs, size_t n_questions,
                            const struct ask_result *result)
{
    struct buf out;
    buf_init(&out);
    for (size_t i = 0; i < result->n_answers; i++) {
        const struct ask_answer *answer = &result->answers[i];
        char *line = ask_format_answer_line(answer_label(specs, n_questions, answer, i), answer);
        if (i)
            buf_append_str(&out, "\n");
        buf_append_str(&out, line);
        free(line);
    }
    return buf_steal(&out);
}

static char *run(const char *args_json, struct tool_run_ctx *ctx)
{
    (void)ctx;

    json_error_t json_error;
    json_t *root = json_loads(args_json ? args_json : "{}", 0, &json_error);
    if (!root)
        return xasprintf("invalid arguments: %s", json_error.text);

    char *result = NULL;
    struct ask_question_spec specs[ASK_MAX_QUESTIONS];
    memset(specs, 0, sizeof specs);
    size_t n_questions = 0;

    result = normalize_questions(root, specs, &n_questions);
    if (result)
        goto out;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        result = xstrdup(ASK_NON_INTERACTIVE);
        goto out;
    }

    struct ask_question questions[ASK_MAX_QUESTIONS];
    for (size_t i = 0; i < n_questions; i++) {
        questions[i] = (struct ask_question){
            .id = specs[i].id,
            .label = specs[i].label,
            .prompt = specs[i].prompt,
            .options = specs[i].options,
            .n_options = specs[i].n_options,
            .allow_other = specs[i].allow_other,
        };
    }

    struct ask_result ask_result;
    interrupt_disarm();
    int ran = question_run(questions, n_questions, &ask_result);
    interrupt_clear_requests();
    interrupt_arm();

    if (ran != 0)
        result = xstrdup(ASK_NON_INTERACTIVE);
    else if (ask_result.cancelled)
        result = xstrdup(ASK_CANCELLED);
    else
        result = format_answers(specs, n_questions, &ask_result);
    ask_result_free(&ask_result);

out:
    free_specs(specs);
    json_decref(root);
    return result;
}

/* "2 questions (Scope, Priority)" — malformed arguments fall back to raw JSON. */
static char *format_argument(const char *args_json)
{
    json_t *root = json_loads(args_json ? args_json : "", 0, NULL);
    if (!root)
        return NULL;
    json_t *questions = json_object_get(root, "questions");
    if (!json_is_array(questions) || json_array_size(questions) == 0) {
        json_decref(root);
        return NULL;
    }

    size_t n_questions = json_array_size(questions);
    struct buf out;
    buf_init(&out);
    char prefix[32];
    snprintf(prefix, sizeof prefix, "%zu question%s (", n_questions, n_questions == 1 ? "" : "s");
    buf_append_str(&out, prefix);
    for (size_t i = 0; i < n_questions; i++) {
        json_t *question = json_array_get(questions, i);
        const char *label = json_string_value(json_object_get(question, "label"));
        const char *id = json_string_value(json_object_get(question, "id"));
        if (i)
            buf_append_str(&out, ", ");
        if (label && *label)
            buf_append_str(&out, label);
        else if (id && *id)
            buf_append_str(&out, id);
        else {
            char fallback[16];
            snprintf(fallback, sizeof fallback, "Q%zu", i + 1);
            buf_append_str(&out, fallback);
        }
    }
    buf_append_str(&out, ")");
    json_decref(root);
    return buf_steal(&out);
}

static const char ASK_DESCRIPTION[] =
    "Ask the user one or more questions with typed options. Single question shows a simple "
    "option list; multiple questions show a tab-based interface. Use when you need user input, "
    "preferences, or confirmation.";

static const char ASK_QUESTIONS_SCHEMA[] =
    "{\"type\":\"array\",\"minItems\":1,\"description\":\"Questions to ask the user.\","
    "\"items\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\",\"description\":\"Unique identifier for this question "
    "(defaults to Q1, Q2, ...).\"},"
    "\"label\":{\"type\":\"string\",\"description\":\"Short contextual label for the tab "
    "bar, e.g. 'Scope' (defaults to Q1, Q2).\"},"
    "\"prompt\":{\"type\":\"string\",\"description\":\"The full question text to display.\"},"
    "\"options\":{\"type\":\"array\",\"minItems\":1,\"description\":\"Available options to "
    "choose from.\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\",\"description\":\"Display label for the option.\"},"
    "\"value\":{\"type\":\"string\",\"description\":\"The value returned when selected "
    "(defaults to label).\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"Optional description shown below "
    "the label.\"}},\"required\":[\"label\"]}},"
    "\"allowOther\":{\"type\":\"boolean\",\"description\":\"Allow a 'Type something' free-text "
    "option (default: true).\"}},"
    "\"required\":[\"prompt\",\"options\"]}}";

static const struct tool_param ASK_PARAMS[] = {
    {.name = "questions", .required = 1, .schema_json = ASK_QUESTIONS_SCHEMA},
};

const struct tool TOOL_ASK = {
    .def = {.name = "ask_user_question",
            .description = ASK_DESCRIPTION,
            .params = ASK_PARAMS,
            .n_params = sizeof(ASK_PARAMS) / sizeof(ASK_PARAMS[0])},
    .run = run,
    .display = {.arg_name = "questions", .format_argument = format_argument, .takes_terminal = 1},
};
