/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_CANCEL_H
#define HAX_SYSTEM_CANCEL_H

/* Process-wide latched cancellation, independent of how it was requested. The terminal's bare-Esc
 * watcher is one producer; an embedder's signal handler or worker thread is another. Requests stay
 * latched until cancel_clear_requests(), and abort implies pause.
 *
 * The flags are the whole state and each accessor is one atomic operation, so producers may run on
 * any thread. */

/* Request a pause, escalating to abort when a pause is already latched. This is the repeated-key
 * path: the first press asks a turn to stop at a seam, the second gives up on the seam. */
void cancel_request_pause(void);

/* Request an immediate abort, which also latches pause. */
void cancel_request_abort(void);

int cancel_pause_requested(void);
int cancel_abort_requested(void);

void cancel_clear_requests(void);

#endif /* HAX_SYSTEM_CANCEL_H */
