/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_DIRENT_H
#define HAX_WIN32_DIRENT_H

#include <stdint.h>
#include <windows.h>

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10

struct dirent {
    uint64_t d_ino;
    unsigned char d_type;
    char d_name[1024];
};

typedef struct hax_win32_dir DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);
int dirfd(DIR *directory);
DIR *fdopendir(int fd);

#endif /* HAX_WIN32_DIRENT_H */
