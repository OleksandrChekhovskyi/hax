/* SPDX-License-Identifier: MIT */
#ifndef HAX_INTERRUPT_H
#define HAX_INTERRUPT_H

/*
 * Esc-key interrupt watcher.
 *
 * Owns a background thread that, while *armed*, reads bytes from stdin in
 * non-canonical mode and sets a latched flag when the user presses bare Esc.
 * Bare Esc is distinguished from CSI/SS3 escape sequences (arrow keys, F-keys,
 * etc.) via a short follow-up timeout — \x1b followed by another byte within
 * ESC_TIMEOUT_MS is the start of an escape sequence and is consumed silently;
 * \x1b alone is the user wanting to interrupt.
 *
 * Lifecycle:
 *   interrupt_init() once at startup.
 *   <user enters prompt — disarmed; libedit owns the tty>
 *   interrupt_arm() — raw mode on, watcher reading.
 *   <stream and tools run, periodically check interrupt_requested()>
 *   interrupt_disarm() — canonical mode restored, watcher paused, stdin drained.
 *   <next prompt>
 *
 * Tty safety: at init we capture the current termios as the canonical
 * baseline and register atexit + signal handlers (SIGINT/SIGTERM/SIGHUP/
 * SIGQUIT) that always restore that baseline — even on a panic mid-arm. The
 * baseline is what was in effect when interrupt_init ran, before libedit's
 * first prompt; libedit save/restore around its own readline calls is
 * unaffected.
 *
 * No-tty mode: when stdin or stdout isn't a tty, all entry points become
 * no-ops — there's no Esc to detect on a piped stdin, and putting a
 * non-tty stdin into raw mode is a no-op anyway. interrupt_requested()
 * always returns 0 in that case.
 */

void interrupt_init(void);

/* Begin listening for Esc. Idempotent — safe to call when already armed. */
void interrupt_arm(void);

/* Stop listening, restore canonical termios, drain pending stdin so any
 * leftover bytes the user typed during the armed window don't leak into
 * the next readline(). Idempotent. */
void interrupt_disarm(void);

/* Latched abort flag. Stays set until interrupt_clear(). Safe to call
 * from any thread. */
int interrupt_requested(void);

/* Block briefly while the classifier is mid-decision on a \x1b — i.e.
 * waiting for the CSI/SS3 follow-up byte or the timeout that confirms a
 * bare Esc. Returns once the flag is latched or the pending state
 * resolves, with a small upper bound. Call before any decision that
 * acts on interrupt_requested() and would be hard to undo (running
 * side-effecting tools, sending another model request). */
void interrupt_settle(void);

/* Clear the latched flag. Call before each new arm cycle. */
void interrupt_clear(void);

/* ---------- pure-logic byte classifier (exposed for testing) ----------
 *
 * State machine that consumes input bytes one at a time and decides whether
 * a bare Esc has been confirmed. The watcher loop drives this with real
 * stdin bytes plus a timeout signal; tests can drive it with synthetic
 * sequences and never touch a tty. */

enum interrupt_state {
    IC_IDLE,
    IC_PENDING_ESC, /* saw \x1b, awaiting follow-up byte or timeout */
    IC_CSI,         /* inside \x1b[ ... <final byte 0x40-0x7E> */
    IC_SS3,         /* inside \x1bO <one byte> */
};

struct interrupt_classifier {
    enum interrupt_state state;
};

void interrupt_classifier_init(struct interrupt_classifier *c);

/* Feed one input byte. Returns 1 if a bare-Esc has just been confirmed
 * (caller should latch the abort flag); 0 otherwise. */
int interrupt_classifier_feed(struct interrupt_classifier *c, unsigned char byte);

/* Signal that the post-\x1b follow-up window has elapsed with no further
 * bytes. Returns 1 if this confirms a bare Esc (i.e. we were in
 * PENDING_ESC); 0 otherwise. */
int interrupt_classifier_timeout(struct interrupt_classifier *c);

#endif /* HAX_INTERRUPT_H */
