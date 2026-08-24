/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_TERMIOS_H
#define HAX_SYSTEM_WIN32_INCLUDE_TERMIOS_H

#include <windows.h>

typedef unsigned int tcflag_t;
struct termios {
    DWORD input_mode;
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    unsigned char c_cc[32];
};

#define ICANON    0x0001
#define ECHO      0x0002
#define ISIG      0x0004
#define IEXTEN    0x0008
#define IXON      0x0010
#define ICRNL     0x0020
#define OPOST     0x0040
#define INPCK     0x0080
#define ISTRIP    0x0100
#define BRKINT    0x0200
#define CS8       0x0400
#define VMIN      0
#define VTIME     1
#define TCSANOW   0
#define TCSADRAIN 1
#define TCIFLUSH  0

int tcgetattr(int fd, struct termios *attributes);
int tcsetattr(int fd, int action, const struct termios *attributes);
int tcflush(int fd, int queue);

#endif /* HAX_SYSTEM_WIN32_INCLUDE_TERMIOS_H */
