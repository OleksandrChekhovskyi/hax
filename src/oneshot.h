/* SPDX-License-Identifier: MIT */
#ifndef HAX_ONESHOT_H
#define HAX_ONESHOT_H

#include "agent_core.h"
#include "provider.h"

/* Run a single non-interactive query against `p`. The `prompt` text is
 * sent verbatim as the user message, the model is allowed to call tools
 * (subject to opts.raw, which omits them) until it produces a final
 * assistant message with no further tool calls, and that final text is
 * written to stdout.
 *
 * `max_turns` caps the number of model round-trips so a confused model
 * can't loop indefinitely in a pipeline. Reaching the cap is treated
 * as an error.
 *
 * Returns 0 on a clean finish, 1 on any error (provider error, max-turns
 * exceeded, missing model, etc.). Errors are logged to stderr; stdout
 * carries only the final assistant text so callers can pipe directly. */
int oneshot_run(struct provider *p, const char *prompt, const struct hax_opts *opts, int max_turns);

#endif /* HAX_ONESHOT_H */
