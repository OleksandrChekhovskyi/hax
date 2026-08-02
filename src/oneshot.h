/* SPDX-License-Identifier: MIT */
#ifndef HAX_ONESHOT_H
#define HAX_ONESHOT_H

#include "agent_core.h"
#include "provider.h"

/* Run one non-interactive user turn. The provider may call advertised tools until it produces a
 * final response or reaches max_turns. Assistant messages from the final response are written to
 * stdout; diagnostics go to stderr. Returns 0 on completion and 1 on failure. */
int oneshot_run(struct provider *provider, const char *prompt, const struct hax_opts *options,
                int max_turns);

#endif /* HAX_ONESHOT_H */
