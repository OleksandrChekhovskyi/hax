/* SPDX-License-Identifier: MIT */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>

#include "agent.h"
#include "providers/codex.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "hax: curl_global_init failed\n");
        return 1;
    }

    struct provider *p = codex_provider_new();
    if (!p) {
        curl_global_cleanup();
        return 1;
    }

    int rc = agent_run(p);

    p->destroy(p);
    curl_global_cleanup();
    return rc;
}
