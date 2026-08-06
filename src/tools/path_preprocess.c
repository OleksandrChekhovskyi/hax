/* SPDX-License-Identifier: MIT */
#include "tools/path_preprocess.h"

#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "system/path.h"

char *tool_relativize_path_args(const char *args_json)
{
    if (!args_json)
        return NULL;

    json_t *root = json_loads(args_json, 0, NULL);
    if (!root)
        return NULL;

    char *rewritten_args = NULL;
    const char *path = json_string_value(json_object_get(root, "path"));
    if (!path)
        goto out;

    char *expanded_path = path_expand_home(path);
    char cwd[PATH_MAX];
    char *relative_path = NULL;
    if (getcwd(cwd, sizeof(cwd)))
        relative_path = path_relativize(expanded_path, cwd);
    free(expanded_path);
    if (!relative_path)
        goto out;

    json_object_set_new(root, "path", json_string(relative_path));
    free(relative_path);
    rewritten_args = json_dumps(root, JSON_COMPACT);

out:
    json_decref(root);
    return rewritten_args;
}
