/* SPDX-License-Identifier: MIT */
#ifndef HAX_WIN32_SYS_IOCTL_H
#define HAX_WIN32_SYS_IOCTL_H

#define TIOCGWINSZ 1
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
int ioctl(int fd, int request, ...);

#endif /* HAX_WIN32_SYS_IOCTL_H */
