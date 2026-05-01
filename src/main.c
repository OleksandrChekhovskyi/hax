/* SPDX-License-Identifier: MIT */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "agent.h"
#include "providers/codex.h"
#include "providers/llamacpp.h"
#include "providers/openai.h"
#include "providers/openai_compat.h"
#include "providers/openrouter.h"

/* Registry of available providers. First entry is the default when
 * HAX_PROVIDER is unset. Adding a new preset = drop a file under
 * src/providers/ and append its PROVIDER_* symbol here. */
static const struct provider_factory *const PROVIDERS[] = {
    &PROVIDER_CODEX,    &PROVIDER_OPENAI,     &PROVIDER_OPENAI_COMPAT,
    &PROVIDER_LLAMACPP, &PROVIDER_OPENROUTER,
};
#define N_PROVIDERS (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

static struct provider *pick_provider(void)
{
    const char *which = getenv("HAX_PROVIDER");
    if (!which || !*which)
        return PROVIDERS[0]->new();

    for (size_t i = 0; i < N_PROVIDERS; i++) {
        if (strcmp(which, PROVIDERS[i]->name) == 0)
            return PROVIDERS[i]->new();
    }

    fprintf(stderr, "hax: unknown HAX_PROVIDER='%s' (supported:", which);
    for (size_t i = 0; i < N_PROVIDERS; i++)
        fprintf(stderr, " %s", PROVIDERS[i]->name);
    fprintf(stderr, ")\n");
    return NULL;
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
