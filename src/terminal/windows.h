/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_WINDOWS_H
#define HAX_TERMINAL_WINDOWS_H

#ifdef _WIN32

enum terminal_stream_kind {
    TERMINAL_STREAM_CONSOLE,
    TERMINAL_STREAM_MSYS_PTY,
    TERMINAL_STREAM_REDIRECTED,
};

/* Classify a CRT descriptor without changing its console mode. */
enum terminal_stream_kind terminal_windows_stream_kind(int fd);

#endif /* _WIN32 */
#endif /* HAX_TERMINAL_WINDOWS_H */
