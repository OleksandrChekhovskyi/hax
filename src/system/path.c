/* SPDX-License-Identifier: MIT */
#include "system/path.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static size_t path_len_without_trailing_slashes(const char *path)
{
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/')
        len--;
    return len;
}

char *path_join(const char *base, const char *suffix)
{
    size_t base_len = path_len_without_trailing_slashes(base);
    while (*suffix == '/')
        suffix++;

    int base_is_root = base_len == 1 && base[0] == '/';
    struct buf joined;
    buf_init(&joined);
    buf_append(&joined, base, base_len);
    if (!base_is_root)
        buf_append_str(&joined, "/");
    buf_append_str(&joined, suffix);
    return buf_steal(&joined);
}

char *path_expand_home(const char *path)
{
    if (!path)
        return NULL;
    if (path[0] != '~' || (path[1] != '\0' && path[1] != '/'))
        return xstrdup(path);

    const char *home = getenv("HOME");
    if (!home || !*home)
        return xstrdup(path);
    if (path[1] == '\0')
        return xstrdup(home);
    return path_join(home, path + 2);
}

char *path_collapse_home(const char *path)
{
    if (!path)
        return NULL;

    const char *home = getenv("HOME");
    if (!home || !*home)
        return xstrdup(path);

    size_t home_len = path_len_without_trailing_slashes(home);
    if (home_len == 1 && home[0] == '/') {
        if (path[0] != '/')
            return xstrdup(path);
        if (path[1] == '\0')
            return xstrdup("~");
        return xasprintf("~%s", path);
    }

    if (strncmp(path, home, home_len) != 0)
        return xstrdup(path);
    if (path[home_len] == '\0')
        return xstrdup("~");
    if (path[home_len] != '/')
        return xstrdup(path);
    return xasprintf("~%s", path + home_len);
}

static int path_has_parent_component(const char *path)
{
    const char *component = path;

    while (*component) {
        while (*component == '/')
            component++;
        const char *end = strchr(component, '/');
        size_t len = end ? (size_t)(end - component) : strlen(component);
        if (len == 2 && component[0] == '.' && component[1] == '.')
            return 1;
        if (!end)
            break;
        component = end + 1;
    }
    return 0;
}

char *path_relativize(const char *path, const char *cwd)
{
    if (!path || !cwd || path[0] != '/' || cwd[0] != '/')
        return NULL;
    if (path_has_parent_component(path))
        return NULL;

    size_t cwd_len = path_len_without_trailing_slashes(cwd);
    const char *relative;
    if (cwd_len == 1) {
        relative = path + 1;
    } else {
        if (strncmp(path, cwd, cwd_len) != 0 || path[cwd_len] != '/')
            return NULL;
        relative = path + cwd_len + 1;
    }

    while (*relative == '/')
        relative++;
    return *relative ? xstrdup(relative) : NULL;
}
