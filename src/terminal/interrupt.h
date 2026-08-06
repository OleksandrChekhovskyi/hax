/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_INTERRUPT_H
#define HAX_TERMINAL_INTERRUPT_H

/* Initialize bare-Esc detection and terminal restoration. Detection is available only when stdin
 * and stdout are TTYs; otherwise the watcher API is inert and request queries return false. */
void interrupt_init(void);

/* Install fatal-signal handlers without starting the TTY watcher. The handlers invoke the optional
 * hook, restore terminal state, and re-raise the signal with its default disposition. */
void interrupt_install_fatal_signal_handlers(void);

/* The hook runs from a signal handler and must be async-signal-safe. */
void interrupt_set_fatal_signal_hook(void (*hook)(void));

/* Start bare-Esc detection. The first Esc requests a pause and the second requests an immediate
 * abort. Arming is idempotent and does not clear requests left by an earlier cycle. */
void interrupt_arm(void);

/* Stop detection, wait until the watcher no longer owns stdin, restore terminal mode, and discard
 * input received while armed. Disarming is idempotent. */
void interrupt_disarm(void);

/* Requests remain latched until interrupt_clear_requests(). Abort implies pause. These queries are
 * safe from any thread. */
int interrupt_pause_requested(void);
int interrupt_abort_requested(void);
void interrupt_clear_requests(void);

/* Wait briefly for a recently received Esc to be classified. Call before an irreversible decision
 * based on the request flags, such as starting a tool or sending another model request. */
void interrupt_resolve_pending_escape(void);

/* Byte classifier exposed so its terminal-independent behavior can be tested directly. */
enum interrupt_classifier_state {
    INTERRUPT_CLASSIFIER_IDLE,
    INTERRUPT_CLASSIFIER_ESCAPE_PENDING,
    INTERRUPT_CLASSIFIER_CSI,
    INTERRUPT_CLASSIFIER_SS3,
};

struct interrupt_classifier {
    enum interrupt_classifier_state state;
};

void interrupt_classifier_init(struct interrupt_classifier *classifier);

/* Return true when this byte confirms that the preceding Esc was bare. */
int interrupt_classifier_feed(struct interrupt_classifier *classifier, unsigned char byte);

/* Return true when the timeout confirms a pending Esc was bare. */
int interrupt_classifier_timeout(struct interrupt_classifier *classifier);

#endif /* HAX_TERMINAL_INTERRUPT_H */
