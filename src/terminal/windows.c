/* SPDX-License-Identifier: MIT */
#ifdef _WIN32

#include "terminal/windows.h"

#include <io.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

static int msys_pty_name(HANDLE handle)
{
    if (GetFileType(handle) != FILE_TYPE_PIPE)
        return 0;

    DWORD capacity = 4096;
    FILE_NAME_INFO *name = malloc(capacity);
    if (!name)
        return 0;
    int result = 0;
    if (GetFileInformationByHandleEx(handle, FileNameInfo, name, capacity)) {
        size_t chars = name->FileNameLength / sizeof(wchar_t);
        wchar_t *copy = malloc((chars + 1) * sizeof(*copy));
        if (copy) {
            for (size_t i = 0; i < chars; i++) {
                wchar_t value = name->FileName[i];
                copy[i] = value >= L'A' && value <= L'Z' ? value - L'A' + L'a' : value;
            }
            copy[chars] = L'\0';
            result = wcsstr(copy, L"msys-") && wcsstr(copy, L"-pty");
            free(copy);
        }
    }
    free(name);
    return result;
}

enum terminal_stream_kind terminal_windows_stream_kind(int fd)
{
    intptr_t raw = _get_osfhandle(fd);
    if (raw == -1)
        return TERMINAL_STREAM_REDIRECTED;
    HANDLE handle = (HANDLE)raw;
    DWORD mode;
    if (GetConsoleMode(handle, &mode))
        return TERMINAL_STREAM_CONSOLE;
    if (msys_pty_name(handle))
        return TERMINAL_STREAM_MSYS_PTY;
    return TERMINAL_STREAM_REDIRECTED;
}

#endif /* _WIN32 */
