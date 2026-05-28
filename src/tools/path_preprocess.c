/* SPDX-License-Identifier: MIT */
#include "tools/path_preprocess.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <jansson.h>

#include "system/path.h"

char *tool_normalize_path_args(const char *args_json)
{
    if (!args_json)
        return NULL;

    json_error_t jerr;
    json_t *root = json_loads(args_json, 0, &jerr);
    if (!root)
        return NULL;

    char *out = NULL;
    const char *raw = json_string_value(json_object_get(root, "path"));
    if (!raw)
        goto done;

    /* expand_home first so a `~`-rooted path can be compared against cwd;
     * leaves anything else untouched. */
    char *abs = expand_home(raw);
    char cwd[PATH_MAX];
    char *rel = NULL;
    if (getcwd(cwd, sizeof(cwd)))
        rel = path_relativize(abs, cwd);
    free(abs);
    if (!rel)
        goto done;

    json_object_set_new(root, "path", json_string(rel));
    free(rel);
    out = json_dumps(root, JSON_COMPACT);

done:
    json_decref(root);
    return out;
}
