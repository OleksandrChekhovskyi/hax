/* SPDX-License-Identifier: MIT */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "agent.h"
#include "providers/codex.h"
#include "providers/openai.h"

static struct provider *pick_provider(void)
{
    const char *which = getenv("HAX_PROVIDER");
    if (which && strcmp(which, "openai") == 0)
        return openai_provider_new();
    if (which && *which && strcmp(which, "codex") != 0) {
        fprintf(stderr, "hax: unknown HAX_PROVIDER='%s' (supported: codex, openai)\n", which);
        return NULL;
    }
    return codex_provider_new();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Must run before anything that emits or decodes multibyte text —
     * libedit in particular crashes on non-UTF-8 LC_CTYPE if our prompt
     * carries any UTF-8 byte. Touches LC_CTYPE only; LC_NUMERIC etc.
     * stay at the C locale so printf output remains predictable. */
    locale_init_utf8();

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "hax: curl_global_init failed\n");
        return 1;
    }

    struct provider *p = pick_provider();
    if (!p) {
        curl_global_cleanup();
        return 1;
    }

    int rc = agent_run(p);

    p->destroy(p);
    curl_global_cleanup();
    return rc;
}
