/* SPDX-License-Identifier: MIT */
#ifndef HAX_LOOP_H
#define HAX_LOOP_H

#include <stddef.h>

#define LOOP_MIN_INTERVAL_MS         60000L
#define LOOP_MAX_INTERVAL_MS         (7L * 24L * 60L * 60L * 1000L)
#define LOOP_SELF_PACED_MIN_DELAY_MS 60000L
#define LOOP_SELF_PACED_MAX_DELAY_MS 3600000L
#define LOOP_EXPIRY_MS               (7L * 24L * 60L * 60L * 1000L)
#define LOOP_FALLBACK_DELAY_MS       (20L * 60L * 1000L)
#define LOOP_MAX_TASKS               50

#define LOOP_DEFAULT_PROMPT                                                                        \
    "Continue any unfinished work from the conversation. Check the current pull request "          \
    "for review comments, failed CI, and merge conflicts. If nothing is pending, look "            \
    "for a small cleanup or simplification. Do not start unrelated work."

struct loop_spec {
    int self_paced;
    long interval_ms;
    const char *prompt; /* borrowed from the parsed argument; NULL selects the default */
};

struct loop_task_info {
    size_t id;
    long interval_ms;
    long next_due_ms;
    long remaining_ms;
    int self_paced;
    const char *prompt;
};

struct loop_schedule;

struct loop_schedule *loop_schedule_new(void);
void loop_schedule_free(struct loop_schedule *schedule);
void loop_schedule_clear(struct loop_schedule *schedule);

int loop_parse_interval(const char *text, long *interval_ms);
int loop_parse_spec(const char *argument, struct loop_spec *spec);

int loop_schedule_add(struct loop_schedule *schedule, const char *prompt, int self_paced,
                      long interval_ms, size_t *id_out);
size_t loop_schedule_count(const struct loop_schedule *schedule);
int loop_schedule_info(const struct loop_schedule *schedule, size_t index, long now_ms,
                       struct loop_task_info *out);
int loop_schedule_stop(struct loop_schedule *schedule, size_t id);
int loop_schedule_stop_all(struct loop_schedule *schedule);
int loop_schedule_cancel_next(struct loop_schedule *schedule);

/* Set the next wakeup for the active self-paced task, or remove it when `stop` is nonzero. */
int loop_schedule_control(struct loop_schedule *schedule, long delay_ms, int stop);

/* Return the next absolute monotonic deadline, or 0 when no task is waiting. */
long loop_schedule_deadline(struct loop_schedule *schedule, long now_ms);

/* Mark the earliest due task active and return its borrowed prompt. */
const char *loop_schedule_take_due(struct loop_schedule *schedule, long now_ms, int *self_paced);

/* Finish the active iteration. A self-paced task that did not choose a delay gets the fallback. */
int loop_schedule_finish(struct loop_schedule *schedule, long now_ms, long *fallback_delay_ms);

#endif /* HAX_LOOP_H */
