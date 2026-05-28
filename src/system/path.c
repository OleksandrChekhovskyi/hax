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

/* True if `path` contains a ".." path component (e.g. "/a/../b", "/a/.."),
 * not merely ".." inside a name like "a..b". */
static int path_has_dotdot(const char *path)
{
    for (const char *s = path; (s = strstr(s, "..")); s += 2) {
        char before = s == path ? '/' : s[-1];
        char after = s[2];
        if (before == '/' && (after == '/' || after == '\0'))
            return 1;
    }
    return 0;
}

char *path_relativize(const char *path, const char *cwd)
{
    if (!path || !cwd || path[0] != '/' || cwd[0] != '/')
        return NULL;
    /* A ".." component can lexically escape cwd ("/repo/../x") or just
     * produce a misleading "a/../x" label. We don't resolve dot segments
     * (see the header note), so bail and leave the original path. */
    if (path_has_dotdot(path))
        return NULL;
    size_t clen = strlen(cwd);
    /* Strip trailing '/' on cwd so "/proj/" matches "/proj" + "/x". */
    while (clen > 1 && cwd[clen - 1] == '/')
        clen--;
    /* cwd is root "/": every absolute path other than "/" itself is
     * under it, and the separating slash sits at position 0. */
    if (clen == 1) {
        const char *rest = path + 1;
        while (*rest == '/')
            rest++;
        return *rest ? xstrdup(rest) : NULL;
    }
    if (strncmp(path, cwd, clen) != 0)
        return NULL;
    /* Require a component boundary so "/proj2/x" doesn't match "/proj"
     * — path[clen] must be the separating '/'. path == cwd (path[clen]
     * == '\0') yields nothing to relativize. */
    if (path[clen] != '/')
        return NULL;
    const char *rest = path + clen + 1;
    while (*rest == '/')
        rest++;
    return *rest ? xstrdup(rest) : NULL;
}
