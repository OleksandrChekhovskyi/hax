/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_NOTIFY_H
#define HAX_TERMINAL_NOTIFY_H

#include <stdio.h>

/* What the interactive REPL is doing, as reported to the terminal. */
enum notify_state {
    NOTIFY_STATE_WORKING, /* a sent prompt or a /compact is being worked on */
    NOTIFY_STATE_BLOCKED, /* the work stopped short and waits for the user to continue it */
    NOTIFY_STATE_DONE,    /* the work is complete and its result possibly unseen */
    NOTIFY_STATE_ERROR,   /* the work failed */
    NOTIFY_STATE_IDLE,    /* awaiting the next prompt, with nothing unseen */
    NOTIFY_STATE_CLEAR,   /* withdraw any reported state before exit */
};

/* The functions below write to stdout and flush. Status reports need an interactive session, with
 * stdin and stdout both terminals; asking for attention needs only stdout to be one. */

/* Report `state` as an OSC 7501 program status, as the `program_status` setting allows. While
 * reports are off, the first call withdraws a record reported earlier. Reports carry only the
 * state, never text from the conversation. */
void notify_status(enum notify_state state);

/* Report the current state again, applying a changed `program_status` setting at once. */
void notify_status_refresh(void);

/* Withdraw the record notify_status() left on the terminal, if any, for exits and stops that skip
 * the normal reports; notify_status_reassert() restores it after a stop. Async-signal-safe. */
void notify_status_withdraw(void);
void notify_status_reassert(void);

/* Report IDLE when the current state is BLOCKED, DONE, or ERROR: the user has now seen it. */
void notify_acknowledge(void);

/* Ask for the user's attention as the `notify` setting selects: a bell, or an OSC 9 desktop
 * notification showing `message`, free text or NULL, reduced to one short line. Only `bel` asks
 * while the terminal has focus. */
void notify_attention(const char *message);

/* Write the bytes notify_status(), notify_status_refresh(), and notify_attention() would emit to
 * `out`, without the terminal check. */
void notify_status_write(FILE *out, enum notify_state state);
void notify_status_refresh_write(FILE *out);
void notify_attention_write(FILE *out, const char *message);

#endif /* HAX_TERMINAL_NOTIFY_H */
