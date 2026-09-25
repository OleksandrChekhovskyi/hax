/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <string.h>

#include "harness.h"
#include "loop.h"

static void test_parse_interval(void)
{
    long interval = 0;
    EXPECT(loop_parse_interval("5m", &interval) == 0 && interval == 5 * 60000L);
    EXPECT(loop_parse_interval("30s", &interval) == 0 && interval == 60000L);
    EXPECT(loop_parse_interval("every 2 hours", &interval) == 0 && interval == 2 * 3600000L);
    EXPECT(loop_parse_interval("1 day", &interval) == 0 && interval == 86400000L);
    EXPECT(loop_parse_interval("7m", &interval) == 0 && interval == 7 * 60000L);

    EXPECT(loop_parse_interval("30s", &interval) == 0);
    EXPECT(loop_parse_interval("0m", &interval) != 0);
    EXPECT(loop_parse_interval("500ms", &interval) != 0);
    EXPECT(loop_parse_interval("8d", &interval) != 0);
    EXPECT(loop_parse_interval("5m extra", &interval) != 0);
    EXPECT(loop_parse_interval(NULL, &interval) != 0);
}

static void test_parse_spec(void)
{
    struct loop_spec spec;
    EXPECT(loop_parse_spec(NULL, &spec) == 0 && spec.self_paced && spec.interval_ms == 0 &&
           spec.prompt == NULL);
    EXPECT(loop_parse_spec("  check CI  ", &spec) == 0 && spec.self_paced && spec.prompt &&
           strcmp(spec.prompt, "check CI  ") == 0);
    EXPECT(loop_parse_spec("5m check CI", &spec) == 0 && !spec.self_paced &&
           spec.interval_ms == 5 * 60000L && strcmp(spec.prompt, "check CI") == 0);
    EXPECT(loop_parse_spec("every 2 hours check CI", &spec) == 0 && !spec.self_paced &&
           spec.interval_ms == 2 * 3600000L && strcmp(spec.prompt, "check CI") == 0);
    EXPECT(loop_parse_spec("5m", &spec) == 0 && !spec.self_paced && spec.prompt == NULL);
}

static void test_schedule_lifecycle(void)
{
    struct loop_schedule *schedule = loop_schedule_new();
    size_t fixed_id = 0;
    size_t adaptive_id = 0;
    EXPECT(loop_schedule_add(schedule, "check", 0, 60000L, &fixed_id) == 0);
    EXPECT(loop_schedule_add(schedule, "polling", 1, 0, &adaptive_id) == 0);
    EXPECT(fixed_id != 0 && adaptive_id != 0 && fixed_id != adaptive_id);
    EXPECT(loop_schedule_count(schedule) == 2);

    struct loop_task_info info;
    EXPECT(loop_schedule_info(schedule, 0, 0, &info) == 0 && info.id == fixed_id);
    long deadline = loop_schedule_deadline(schedule, 0);
    EXPECT(deadline > 0);
    int self_paced = -1;
    const char *prompt = loop_schedule_take_due(schedule, deadline, &self_paced);
    EXPECT(prompt != NULL && strcmp(prompt, "polling") == 0 && self_paced);
    EXPECT(loop_schedule_take_due(schedule, deadline, &self_paced) == NULL);
    long fallback = 0;
    EXPECT(loop_schedule_finish(schedule, deadline, &fallback) != 0);
    EXPECT(fallback == LOOP_FALLBACK_DELAY_MS);

    EXPECT(loop_schedule_info(schedule, 0, deadline, &info) == 0 && info.id == fixed_id);
    deadline = loop_schedule_deadline(schedule, deadline);
    EXPECT(deadline > 0);
    prompt = loop_schedule_take_due(schedule, deadline, &self_paced);
    EXPECT(prompt != NULL && strcmp(prompt, "check") == 0 && !self_paced);
    EXPECT(loop_schedule_finish(schedule, deadline, &fallback) == 0 && fallback == 0);

    EXPECT(loop_schedule_stop(schedule, fixed_id) == 0);
    EXPECT(loop_schedule_stop(schedule, fixed_id) != 0);
    EXPECT(loop_schedule_count(schedule) == 1);
    EXPECT(loop_schedule_cancel_next(schedule) == (int)adaptive_id);
    EXPECT(loop_schedule_count(schedule) == 0);

    loop_schedule_free(schedule);
    loop_schedule_free(NULL);
}

static void test_schedule_clear_and_limits(void)
{
    struct loop_schedule *schedule = loop_schedule_new();
    for (size_t i = 0; i < LOOP_MAX_TASKS; i++)
        EXPECT(loop_schedule_add(schedule, "x", 0, LOOP_MIN_INTERVAL_MS, NULL) == 0);
    EXPECT(loop_schedule_add(schedule, "overflow", 0, LOOP_MIN_INTERVAL_MS, NULL) != 0);
    EXPECT(loop_schedule_stop_all(schedule) == 0);
    EXPECT(loop_schedule_count(schedule) == 0);
    EXPECT(loop_schedule_cancel_next(schedule) != 0);
    loop_schedule_free(schedule);
}

int main(void)
{
    test_parse_interval();
    test_parse_spec();
    test_schedule_lifecycle();
    test_schedule_clear_and_limits();
    T_REPORT();
}
