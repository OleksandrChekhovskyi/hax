/* SPDX-License-Identifier: MIT */
#include "tools/bash_shell.h"

#include <unistd.h>

#include "config.h"
#include "diag.h"
#include "xalloc.h"
#include "system/fs.h"

char *bash_resolve_shell(void)
{
    const char *configured_shell = config_str("bash.shell");
    if (configured_shell && *configured_shell) {
        char *shell_path = fs_which(configured_shell);
        if (shell_path)
            return shell_path;
        static int warned;
        if (!warned) {
            warned = 1;
            hax_warn("bash.shell: '%s' not found or not executable; using default",
                     configured_shell);
        }
    }

    char *shell_path = fs_which("bash");
    if (shell_path)
        return shell_path;
    if (access("/bin/bash", X_OK) == 0)
        return xstrdup("/bin/bash");
    return xstrdup("/bin/sh");
}
