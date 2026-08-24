/* SPDX-License-Identifier: MIT */
#include "system/browser.h"

#include <stddef.h>

#include "system/spawn.h"

void browser_open_url(const char *url)
{
    /* xdg-open's generic fallback execs the browser directly and lives as long as it, which is
     * why the spawn must be detached rather than waited on or killed. */
#ifdef __APPLE__
    const char *argv[] = {"open", url, NULL};
#else
    const char *argv[] = {"xdg-open", url, NULL};
#endif
    spawn_detached(argv);
}
