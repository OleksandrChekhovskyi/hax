/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_FOCUS_H
#define HAX_TERMINAL_FOCUS_H

/* Terminal focus as last reported through DEC mode 1004. Until a report arrives, focus is
 * assumed: many terminals report only changes, and a bell for a watching user costs more than a
 * missed one. */
enum focus_state {
    FOCUS_IN,
    FOCUS_OUT,
};

/* Toggle focus reporting on stdout. Enable it only while a raw-mode reader consumes stdin: with
 * canonical-mode echo, the tty would print the reports. Disabling keeps the last known state. */
void focus_reporting_enable(void);
void focus_reporting_disable(void);

/* Async-signal-safe, for a stop: turn enabled reporting off without forgetting that it was on, and
 * return whether it was; focus_reporting_resume() turns it back on. */
int focus_reporting_suspend(void);
void focus_reporting_resume(void);

/* Record a focus report decoded from input. Safe from any thread. */
void focus_record(enum focus_state state);

/* The last recorded state. Safe from any thread. */
enum focus_state focus_current(void);

#endif /* HAX_TERMINAL_FOCUS_H */
