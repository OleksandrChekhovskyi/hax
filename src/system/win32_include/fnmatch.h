/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_FNMATCH_H
#define HAX_SYSTEM_WIN32_INCLUDE_FNMATCH_H

#define FNM_NOMATCH 1
int fnmatch(const char *pattern, const char *text, int flags);

#endif /* HAX_SYSTEM_WIN32_INCLUDE_FNMATCH_H */
