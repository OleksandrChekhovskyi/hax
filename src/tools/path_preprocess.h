/* SPDX-License-Identifier: MIT */
#ifndef HAX_PATH_PREPROCESS_H
#define HAX_PATH_PREPROCESS_H

/* Rewrite an absolute `path` under the process working directory as relative JSON arguments.
 * Returns allocated JSON, or NULL when the original arguments should be used. */
char *tool_relativize_path_args(const char *args_json);

#endif /* HAX_PATH_PREPROCESS_H */
