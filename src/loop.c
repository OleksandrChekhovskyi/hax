/* SPDX-License-Identifier: MIT */
#include "loop.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"
#include "system/clock.h"

struct loop_task {
    size_t id;
    char *prompt;
    long interval_ms;
    long next_due_ms;
    long expires_at_ms;
    int self_paced;
    int continuation_set;
};

struct loop_schedule {
    struct loop_task tasks[LOOP_MAX_TASKS];
    size_t count;
    size_t next_id;
    size_t active;
};

static const char *skip_space(const char *text)
{
    while (*text && isspace((unsigned char)*text))
        text++;
    return text;
}

static int equal_word(const char *text, size_t length, const char *word)
{
    size_t word_length = strlen(word);
    if (length != word_length)
        return 0;
    for (size_t i = 0; i < length; i++)
        if (tolower((unsigned char)text[i]) != word[i])
            return 0;
    return 1;
}

static int unit_milliseconds(const char *text, size_t length, long *multiplier)
{
    static const struct {
        const char *name;
        long milliseconds;
    } units[] = {
        {"s", 1000L},       {"sec", 1000L},      {"secs", 1000L},     {"second", 1000L},
        {"seconds", 1000L}, {"m", 60000L},       {"min", 60000L},     {"mins", 60000L},
        {"minute", 60000L}, {"minutes", 60000L}, {"h", 3600000L},     {"hr", 3600000L},
        {"hrs", 3600000L},  {"hour", 3600000L},  {"hours", 3600000L}, {"d", 86400000L},
        {"day", 86400000L}, {"days", 86400000L},
    };

    if (length == 0)
        return -1;
    if (length > 1 && text[length - 1] == 's') {
        char *without_plural = xstrdup(text);
        for (size_t i = 0; i < length - 1; i++)
            without_plural[i] = (char)tolower((unsigned char)without_plural[i]);
        int found = 0;
        for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
            if (strcmp(without_plural, units[i].name) == 0) {
                *multiplier = units[i].milliseconds;
                found = 1;
                break;
            }
        }
        free(without_plural);
        if (found)
            return 0;
    }

    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        size_t unit_length = strlen(units[i].name);
        if (length == unit_length) {
            int matches = 1;
            for (size_t j = 0; j < length; j++)
                if (tolower((unsigned char)text[j]) != units[i].name[j]) {
                    matches = 0;
                    break;
                }
            if (matches) {
                *multiplier = units[i].milliseconds;
                return 0;
            }
        }
    }
    return -1;
}

static int parse_interval_prefix(const char *text, long *interval_ms, const char **end_out)
{
    const char *cursor = skip_space(text);
    size_t word_length = 0;
    while (cursor[word_length] && !isspace((unsigned char)cursor[word_length]))
        word_length++;
    if (word_length == 5 && equal_word(cursor, word_length, "every")) {
        cursor += word_length;
        cursor = skip_space(cursor);
    }

    if (!isdigit((unsigned char)*cursor))
        return -1;
    errno = 0;
    char *number_end = NULL;
    unsigned long long value = strtoull(cursor, &number_end, 10);
    if (errno == ERANGE || number_end == cursor)
        return -1;
    cursor = number_end;
    cursor = skip_space(cursor);

    size_t unit_start = 0;
    while (cursor[unit_start] && !isspace((unsigned char)cursor[unit_start]))
        unit_start++;
    long multiplier = 0;
    if (unit_milliseconds(cursor, unit_start, &multiplier) != 0)
        return -1;
    cursor += unit_start;

    const char *end = skip_space(cursor);
    if (value == 0 || value > (unsigned long long)LONG_MAX / (unsigned long long)multiplier)
        return -1;
    unsigned long long milliseconds = value * (unsigned long long)multiplier;
    if (multiplier == 1000L)
        milliseconds = (milliseconds + 59999ULL) / 60000ULL * 60000ULL;
    if (milliseconds < (unsigned long long)LOOP_MIN_INTERVAL_MS ||
        milliseconds > (unsigned long long)LOOP_MAX_INTERVAL_MS)
        return -1;

    *interval_ms = (long)milliseconds;
    *end_out = end;
    return 0;
}

int loop_parse_interval(const char *text, long *interval_ms)
{
    const char *end = NULL;
    if (!text || !interval_ms || parse_interval_prefix(text, interval_ms, &end) != 0)
        return -1;
    return *end == '\0' ? 0 : -1;
}

int loop_parse_spec(const char *argument, struct loop_spec *spec)
{
    if (!spec)
        return -1;
    memset(spec, 0, sizeof(*spec));
    spec->self_paced = 1;
    const char *text = skip_space(argument ? argument : "");
    if (*text == '\0')
        return 0;

    long interval_ms = 0;
    const char *end = NULL;
    if (parse_interval_prefix(text, &interval_ms, &end) == 0) {
        spec->self_paced = 0;
        spec->interval_ms = interval_ms;
        spec->prompt = *end ? end : NULL;
        return 0;
    }
    spec->self_paced = 1;
    spec->prompt = text;
    return 0;
}

struct loop_schedule *loop_schedule_new(void)
{
    struct loop_schedule *schedule = xcalloc(1, sizeof(*schedule));
    schedule->active = LOOP_MAX_TASKS;
    return schedule;
}

static void task_free(struct loop_task *task)
{
    free(task->prompt);
    memset(task, 0, sizeof(*task));
}

void loop_schedule_clear(struct loop_schedule *schedule)
{
    if (!schedule)
        return;
    for (size_t i = 0; i < schedule->count; i++)
        task_free(&schedule->tasks[i]);
    schedule->count = 0;
    schedule->active = LOOP_MAX_TASKS;
    schedule->next_id = 0;
}

void loop_schedule_free(struct loop_schedule *schedule)
{
    if (!schedule)
        return;
    loop_schedule_clear(schedule);
    free(schedule);
}

static size_t find_id(const struct loop_schedule *schedule, size_t id)
{
    for (size_t i = 0; i < schedule->count; i++)
        if (schedule->tasks[i].id == id)
            return i;
    return LOOP_MAX_TASKS;
}

static size_t next_id(struct loop_schedule *schedule)
{
    for (;;) {
        schedule->next_id++;
        if (schedule->next_id == 0)
            schedule->next_id = 1;
        if (find_id(schedule, schedule->next_id) == LOOP_MAX_TASKS)
            return schedule->next_id;
    }
}

int loop_schedule_add(struct loop_schedule *schedule, const char *prompt, int self_paced,
                      long interval_ms, size_t *id_out)
{
    if (!schedule || !prompt || !*prompt || schedule->count >= LOOP_MAX_TASKS)
        return -1;
    if (!self_paced && (interval_ms < LOOP_MIN_INTERVAL_MS || interval_ms > LOOP_MAX_INTERVAL_MS))
        return -1;

    long now = monotonic_ms();
    struct loop_task *task = &schedule->tasks[schedule->count++];
    task->id = next_id(schedule);
    task->prompt = xstrdup(prompt);
    task->interval_ms = self_paced ? 0 : interval_ms;
    task->next_due_ms = self_paced ? now : now + interval_ms;
    task->expires_at_ms = now + LOOP_EXPIRY_MS;
    task->self_paced = self_paced;
    if (id_out)
        *id_out = task->id;
    return 0;
}

size_t loop_schedule_count(const struct loop_schedule *schedule)
{
    return schedule ? schedule->count : 0;
}

int loop_schedule_info(const struct loop_schedule *schedule, size_t index, long now_ms,
                       struct loop_task_info *out)
{
    if (!schedule || !out || index >= schedule->count)
        return -1;
    const struct loop_task *task = &schedule->tasks[index];
    out->id = task->id;
    out->interval_ms = task->interval_ms;
    out->next_due_ms = task->next_due_ms;
    out->remaining_ms = task->next_due_ms > now_ms ? task->next_due_ms - now_ms : 0;
    out->self_paced = task->self_paced;
    out->prompt = task->prompt;
    return 0;
}

static void remove_at(struct loop_schedule *schedule, size_t index)
{
    task_free(&schedule->tasks[index]);
    if (index + 1 < schedule->count)
        memmove(&schedule->tasks[index], &schedule->tasks[index + 1],
                (schedule->count - index - 1) * sizeof(schedule->tasks[0]));
    schedule->count--;
    if (schedule->active == index)
        schedule->active = LOOP_MAX_TASKS;
    else if (schedule->active > index && schedule->active != LOOP_MAX_TASKS)
        schedule->active--;
}

static void remove_expired(struct loop_schedule *schedule, long now_ms)
{
    if (!schedule)
        return;
    for (size_t i = schedule->count; i-- > 0;)
        if (schedule->tasks[i].expires_at_ms <= now_ms)
            remove_at(schedule, i);
}

int loop_schedule_stop(struct loop_schedule *schedule, size_t id)
{
    if (!schedule || id == 0)
        return -1;
    size_t index = find_id(schedule, id);
    if (index == LOOP_MAX_TASKS)
        return -1;
    remove_at(schedule, index);
    return 0;
}

int loop_schedule_stop_all(struct loop_schedule *schedule)
{
    if (!schedule)
        return -1;
    loop_schedule_clear(schedule);
    return 0;
}

int loop_schedule_cancel_next(struct loop_schedule *schedule)
{
    if (!schedule || schedule->count == 0)
        return -1;
    size_t earliest = 0;
    for (size_t i = 1; i < schedule->count; i++)
        if (schedule->tasks[i].next_due_ms < schedule->tasks[earliest].next_due_ms)
            earliest = i;
    size_t id = schedule->tasks[earliest].id;
    remove_at(schedule, earliest);
    return (int)id;
}

int loop_schedule_control(struct loop_schedule *schedule, long delay_ms, int stop)
{
    if (!schedule || schedule->active >= schedule->count)
        return -1;
    struct loop_task *task = &schedule->tasks[schedule->active];
    if (!task->self_paced)
        return -1;
    if (stop) {
        size_t active = schedule->active;
        remove_at(schedule, active);
        return 0;
    }
    if (delay_ms < LOOP_SELF_PACED_MIN_DELAY_MS || delay_ms > LOOP_SELF_PACED_MAX_DELAY_MS)
        return -1;

    long now = monotonic_ms();
    task->next_due_ms = now + delay_ms;
    if (task->next_due_ms >= task->expires_at_ms)
        task->next_due_ms = task->expires_at_ms - 1;
    task->continuation_set = 1;
    return 0;
}

long loop_schedule_deadline(struct loop_schedule *schedule, long now_ms)
{
    if (!schedule)
        return 0;
    remove_expired(schedule, now_ms);
    if (schedule->count == 0 || schedule->active != LOOP_MAX_TASKS)
        return 0;
    long deadline = schedule->tasks[0].next_due_ms;
    for (size_t i = 1; i < schedule->count; i++)
        if (schedule->tasks[i].next_due_ms < deadline)
            deadline = schedule->tasks[i].next_due_ms;
    return deadline;
}

const char *loop_schedule_take_due(struct loop_schedule *schedule, long now_ms, int *self_paced)
{
    if (!schedule || schedule->active != LOOP_MAX_TASKS)
        return NULL;
    remove_expired(schedule, now_ms);
    size_t due = LOOP_MAX_TASKS;
    for (size_t i = 0; i < schedule->count; i++) {
        if (schedule->tasks[i].next_due_ms > now_ms)
            continue;
        if (due == LOOP_MAX_TASKS ||
            schedule->tasks[i].next_due_ms < schedule->tasks[due].next_due_ms)
            due = i;
    }
    if (due == LOOP_MAX_TASKS)
        return NULL;

    struct loop_task *task = &schedule->tasks[due];
    schedule->active = due;
    task->continuation_set = 0;
    if (self_paced)
        *self_paced = task->self_paced;
    if (!task->self_paced)
        task->next_due_ms = now_ms + task->interval_ms;
    return task->prompt;
}

int loop_schedule_finish(struct loop_schedule *schedule, long now_ms, long *fallback_delay_ms)
{
    if (fallback_delay_ms)
        *fallback_delay_ms = 0;
    if (!schedule || schedule->active >= schedule->count)
        return 0;
    struct loop_task *task = &schedule->tasks[schedule->active];
    if (task->expires_at_ms <= now_ms) {
        size_t active = schedule->active;
        remove_at(schedule, active);
        return 0;
    }
    int fallback = task->self_paced && !task->continuation_set;
    if (fallback) {
        task->next_due_ms = now_ms + LOOP_FALLBACK_DELAY_MS;
        if (fallback_delay_ms)
            *fallback_delay_ms = LOOP_FALLBACK_DELAY_MS;
    }
    schedule->active = LOOP_MAX_TASKS;
    return fallback;
}
