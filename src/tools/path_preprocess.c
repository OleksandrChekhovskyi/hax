/* SPDX-License-Identifier: MIT */
#include "tools/path_preprocess.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <jansson.h>

#include "system/path.h"

char *tool_relativize_path_args(const char *args_json)
{
    if (!args_json)
        return NULL;

    json_error_t jerr;
    json_t *root = json_loads(args_json, 0, &jerr);
    if (!root)
        return NULL;

    char *rewritten = NULL;
    const char *path = json_string_value(json_object_get(root, "path"));
    if (!path)
        goto done;

    char *absolute_path = expand_home(path);
    char cwd[PATH_MAX];
    char *relative_path = NULL;
    if (getcwd(cwd, sizeof(cwd)))
        relative_path = path_relativize(absolute_path, cwd);
    free(absolute_path);
    if (!relative_path)
        goto done;

    json_object_set_new(root, "path", json_string(relative_path));
    free(relative_path);
    rewritten = json_dumps(root, JSON_COMPACT);

done:
    json_decref(root);
    return rewritten;
}
