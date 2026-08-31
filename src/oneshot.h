/* SPDX-License-Identifier: MIT */
#ifndef HAX_ONESHOT_H
#define HAX_ONESHOT_H

#include "agent_core.h"
#include "provider.h"

/* Run one non-interactive user turn. The provider may call advertised tools until it produces a
 * final response or reaches the max_turns bound. A NULL `prompt` continues a resumed session
 * from where it stopped instead of adding user input. Assistant messages from the final response
 * are written to stdout; diagnostics go to stderr. Returns 0 on completion, 130 when a stop
 * signal interrupted the run, and 1 on failure or pause. */
int oneshot_run(struct provider *provider, const char *prompt, const struct hax_opts *options);

#endif /* HAX_ONESHOT_H */
