/* SPDX-License-Identifier: MIT */
#include "tools/bash_shell.h"

#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "util.h"
#include "system/fs.h"

/* Prefer bash because model-generated commands commonly use bash syntax. */
char *bash_resolve_shell(void)
{
    const char *cfg = config_str("bash.shell");
    if (cfg && *cfg) {
        char *p = fs_which(cfg);
        if (p)
            return p;
        static int warned;
        if (!warned) {
            warned = 1;
            hax_warn("bash.shell: '%s' not found or not executable; using default", cfg);
        }
    }
    char *p = fs_which("bash");
    if (p)
        return p;
    /* Cover environments where PATH is missing or restricted. */
    if (access("/bin/bash", X_OK) == 0)
        return xstrdup("/bin/bash");
    return xstrdup("/bin/sh");
}
