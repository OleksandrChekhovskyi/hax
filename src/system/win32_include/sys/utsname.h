/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_SYS_UTSNAME_H
#define HAX_SYSTEM_WIN32_INCLUDE_SYS_UTSNAME_H

struct utsname {
    char sysname[64];
    char nodename[256];
    char release[64];
    char version[128];
    char machine[64];
};
int uname(struct utsname *name);

#endif /* HAX_SYSTEM_WIN32_INCLUDE_SYS_UTSNAME_H */
