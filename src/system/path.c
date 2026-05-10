/* SPDX-License-Identifier: MIT */
#include "system/path.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

char *path_join(const char *base, const char *rel)
{
    size_t blen = strlen(base);
    /* Preserve "/" as the root: blen==1 with base[0]=='/' must not be
     * stripped to empty. Any other run of trailing slashes collapses. */
    while (blen > 1 && base[blen - 1] == '/')
        blen--;
    while (*rel == '/')
        rel++;
    /* Special-case base=="/" so we get "/rel" instead of "//rel". */
    if (blen == 1 && base[0] == '/')
        return xasprintf("/%s", rel);
    return xasprintf("%.*s/%s", (int)blen, base, rel);
}

char *expand_home(const char *path)
{
    if (!path)
        return NULL;
    if (path[0] != '~')
        return xstrdup(path);
    /* Only bare `~` and `~/` expand. `~user/...` would need getpwnam,
     * which isn't worth pulling in for an LLM-typed path. */
    if (path[1] != '\0' && path[1] != '/')
        return xstrdup(path);
    const char *home = getenv("HOME");
    if (!home || !*home)
        return xstrdup(path);
    if (path[1] == '\0')
        return xstrdup(home);
    return xasprintf("%s%s", home, path + 1);
}

char *collapse_home(const char *path)
{
    if (!path)
        return NULL;
    const char *home = getenv("HOME");
    if (!home || !*home)
        return xstrdup(path);
    size_t hlen = strlen(home);
    /* Strip any trailing '/' on $HOME so "/foo/" matches "/foo" + "/x". */
    while (hlen > 1 && home[hlen - 1] == '/')
        hlen--;
    /* HOME="/" is degenerate (root running in a container, daemons) but
     * possible. The boundary check below assumes hlen>1 so path[hlen]
     * lands on the separating '/'; with hlen==1 the slash is at position
     * 0 instead, so handle it explicitly. */
    if (hlen == 1 && home[0] == '/') {
        if (path[0] != '/')
            return xstrdup(path);
        if (path[1] == '\0')
            return xstrdup("~");
        return xasprintf("~%s", path);
    }
    if (strncmp(path, home, hlen) != 0)
        return xstrdup(path);
    /* path equals $HOME exactly. */
    if (path[hlen] == '\0')
        return xstrdup("~");
    /* path is $HOME followed by '/...', a real subpath. Without the '/'
     * boundary check "/Users/foobar" would match $HOME="/Users/foo". */
    if (path[hlen] == '/')
        return xasprintf("~%s", path + hlen);
    return xstrdup(path);
}
