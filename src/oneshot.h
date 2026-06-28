/* SPDX-License-Identifier: MIT */
#ifndef HAX_ONESHOT_H
#define HAX_ONESHOT_H

#include "agent_core.h"
#include "provider.h"

/* Run a non-interactive prompt to a final assistant message, allowing tools
 * unless opts.raw omits them. `max_turns` bounds agentic loops. Returns 0 on
 * clean finish; errors go to stderr so stdout stays pipeable. */
int oneshot_run(struct provider *p, const char *prompt, const struct hax_opts *opts, int max_turns);

#endif /* HAX_ONESHOT_H */
