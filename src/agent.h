/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_H
#define HAX_AGENT_H

#include "agent_core.h"
#include "provider.h"

struct transcript_log;

/* Live REPL state owned by agent_run, exposed to slash handlers (and
 * any future callers that need to mutate the conversation) so a single
 * pointer is enough to act on history, the optional HAX_TRANSCRIPT log,
 * and the provider in lockstep. Lifetime: stack-allocated in agent_run;
 * never heap-owned, never outlives the REPL frame. */
struct agent_state {
    struct agent_session *sess;
    const struct provider *provider;
    struct transcript_log *tlog;
};

int agent_run(struct provider *p, const struct hax_opts *opts);

/* Print the two-line startup banner: provider/model identification
 * and the key-tip line. Reused by agent_new_conversation so a fresh-
 * conversation reset shows the same banner the user saw at startup.
 * Emits a leading blank line so the banner stands clear of whatever
 * was on the terminal before. */
void agent_print_banner(const struct provider *p, const struct agent_session *s);

/* Reset the live conversation to a fresh state: clear the items vector,
 * truncate-and-rewrite the HAX_TRANSCRIPT log (no-op when unset), and
 * reprint the banner. The single entry point for "what /new means" so
 * future additions (e.g. clearing a future cache, resetting usage
 * counters) don't leak into slash.c. */
void agent_new_conversation(struct agent_state *st);

#endif /* HAX_AGENT_H */
