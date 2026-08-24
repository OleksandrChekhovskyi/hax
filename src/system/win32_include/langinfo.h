/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_LANGINFO_H
#define HAX_SYSTEM_WIN32_INCLUDE_LANGINFO_H

#define CODESET 0
static inline const char *nl_langinfo(int item)
{
    (void)item;
    return "UTF-8";
}

#endif /* HAX_SYSTEM_WIN32_INCLUDE_LANGINFO_H */
