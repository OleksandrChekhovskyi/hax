/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_FNMATCH_H
#define HAX_WIN32_FNMATCH_H

#define FNM_NOMATCH 1
int fnmatch(const char *pattern, const char *text, int flags);

#endif /* HAX_WIN32_FNMATCH_H */
