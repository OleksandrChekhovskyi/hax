/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_H
#define HAX_AGENT_H

#include "agent_core.h"
#include "provider.h"

int agent_run(struct provider *p, const struct hax_opts *opts);

/* Print the two-line startup banner: provider/model identification
 * and the key-tip line. Reused by `/new` so a fresh-conversation
 * reset shows the same banner the user saw at startup. Emits a
 * leading blank line so the banner stands clear of whatever was on
 * the terminal before. */
void agent_print_banner(const struct provider *p, const struct agent_session *s);

#endif /* HAX_AGENT_H */
