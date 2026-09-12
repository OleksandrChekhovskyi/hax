/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_QUESTION_H
#define HAX_TERMINAL_QUESTION_H

#include <stddef.h>

/* One selectable answer to a question. `value` falls back to `label` when
 * NULL or empty. `description` is optional row detail under the label. */
struct ask_question_option {
    const char *label;
    const char *value;
    const char *description;
};

/* `id` and `label` are optional and default to "Q<n>". `prompt` is required. */
struct ask_question {
    const char *id;
    const char *label;
    const char *prompt;
    const struct ask_question_option *options;
    size_t n_options;
    int allow_other; /* adds a "Type something." free-text option */
};

/* One answer, in the order the questions were presented. `text` is the option
 * label or the typed text, `value` the option value (the typed text itself for
 * free answers); both are owned. `index` is the 1-based option index, -1 for a
 * free answer. `question_id` borrows the matching question's id, or NULL when
 * the question had none. */
struct ask_answer {
    const char *question_id;
    int index;
    char *text;
    char *value;
    int was_custom;
};

struct ask_result {
    int cancelled;
    struct ask_answer *answers; /* owned, one per answered question */
    size_t n_answers;
};

/* Free the answers owned by a result. Safe to call with a zeroed result. */
void ask_result_free(struct ask_result *result);

/* Open a blocking, TTY-only widget for 1..5 questions, mirroring picker_run:
 * it owns raw mode until it returns. `result` is caller-owned and must be
 * freed with ask_result_free(). All input pointers are borrowed for the
 * duration of the call. Return 0 when the widget ran and -1 when it could not
 * (missing arguments, non-TTY, terminal setup failure); a -1 result or a
 * cancelled run sets cancelled = 1 and leaves `answers` NULL. */
int question_run(const struct ask_question *questions, size_t n_questions,
                 struct ask_result *result);

#endif /* HAX_TERMINAL_QUESTION_H */
