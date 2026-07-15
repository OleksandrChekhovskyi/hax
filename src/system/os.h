/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_OS_H
#define HAX_SYSTEM_OS_H

/* Parse a freedesktop os-release file and return malloc'd PRETTY_NAME,
 * falling back to NAME + VERSION; return NULL if neither is available. */
char *os_release_name(const char *path);

/* Read primary_path exclusively when it exists; otherwise read fallback_path. */
char *os_release_name_with_fallback(const char *primary_path, const char *fallback_path);

/* Return a malloc'd userland and kernel description for the Environment section. */
char *os_description(void);

#endif /* HAX_SYSTEM_OS_H */
