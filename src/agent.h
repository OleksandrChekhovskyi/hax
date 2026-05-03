/* SPDX-License-Identifier: MIT */
#ifndef HAX_AGENT_H
#define HAX_AGENT_H

#include "agent_core.h"
#include "provider.h"

int agent_run(struct provider *p, const struct hax_opts *opts);

#endif /* HAX_AGENT_H */
