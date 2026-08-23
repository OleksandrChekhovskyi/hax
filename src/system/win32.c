/* SPDX-License-Identifier: MIT */
#ifdef _WIN32

#include "hax_win32.h"

#undef access
#undef chdir
#undef chmod
#undef getcwd
#undef getline
#undef gmtime_r
#undef kill
#undef lstat
#undef localtime_r
#undef readlink
#undef fclose
#undef fflush
#undef fopen
#undef freopen
#undef mkdir
#undef mkdtemp
#undef mkstemp
#undef open
#undef open_memstream
#undef pipe
#undef fcntl
#undef realpath
#undef remove
#undef rename
#undef rmdir
#undef stat
#undef symlink
#undef unlink
#undef utimensat
#undef wcwidth

#include <bcrypt.h>
#include <direct.h>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <io.h>
#include <libgen.h>
#include <poll.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <wchar.h>
#include <windows.h>
#include <winioctl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/locking.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "util.h"
#include "terminal/windows.h"

static void set_errno_from_win32(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_FILENAME_EXCED_RANGE:
        errno = ENAMETOOLONG;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    default:
        errno = EIO;
        break;
    }
}

static wchar_t *utf8_to_wide(const char *text)
{
    if (!text) {
        errno = EINVAL;
        return NULL;
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        set_errno_from_win32(GetLastError());
        return NULL;
    }
    wchar_t *wide = malloc((size_t)length * sizeof(*wide));
    if (!wide) {
        errno = ENOMEM;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length)) {
        set_errno_from_win32(GetLastError());
        free(wide);
        return NULL;
    }
    return wide;
}

static char *wide_to_utf8(const wchar_t *text)
{
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        set_errno_from_win32(GetLastError());
        return NULL;
    }
    char *utf8 = malloc((size_t)length);
    if (!utf8) {
        errno = ENOMEM;
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8, length, NULL, NULL)) {
        set_errno_from_win32(GetLastError());
        free(utf8);
        return NULL;
    }
    return utf8;
}

static int ascii_alpha(char value)
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

static wchar_t *path_to_wide(const char *path)
{
    if (path && strcmp(path, "/dev/null") == 0)
        return _wcsdup(L"NUL");
    if (path && strcmp(path, "/dev/tty") == 0)
        return _wcsdup(L"CON");
    if (path && strncmp(path, "/tmp", 4) == 0 && (path[4] == '\0' || path[4] == '/')) {
        wchar_t temporary[32768];
        DWORD length = GetTempPathW(32768, temporary);
        wchar_t *suffix = utf8_to_wide(path + 4);
        if (!length || length >= 32768 || !suffix) {
            free(suffix);
            errno = EIO;
            return NULL;
        }
        while (length > 0 && (temporary[length - 1] == L'\\' || temporary[length - 1] == L'/'))
            temporary[--length] = L'\0';
        size_t needed = length + wcslen(suffix) + 1;
        wchar_t *result = malloc(needed * sizeof(*result));
        if (result) {
            wcscpy(result, temporary);
            wcscat(result, suffix);
            for (wchar_t *cursor = result; *cursor; cursor++)
                if (*cursor == L'/')
                    *cursor = L'\\';
        }
        free(suffix);
        return result;
    }
    char *native = NULL;
    if (path && path[0] == '/' && ascii_alpha(path[1]) &&
        (path[2] == '/' || path[2] == '\\' || path[2] == '\0')) {
        size_t length = strlen(path);
        native = malloc(length + 2);
        if (!native) {
            errno = ENOMEM;
            return NULL;
        }
        native[0] = path[1] >= 'a' ? (char)(path[1] - 'a' + 'A') : path[1];
        native[1] = ':';
        memcpy(native + 2, path + 2, length - 1);
    } else if (path && path[0] == '/' && path[1] == '/') {
        native = _strdup(path);
        if (native) {
            native[0] = '\\';
            native[1] = '\\';
        }
    } else if (path && path[0] == '/') {
        errno = ENOTSUP;
        return NULL;
    } else {
        native = path ? _strdup(path) : NULL;
    }
    if (!native) {
        errno = path ? ENOMEM : EINVAL;
        return NULL;
    }
    for (char *cursor = native; *cursor; cursor++)
        if (*cursor == '/')
            *cursor = '\\';
    wchar_t *wide = utf8_to_wide(native);
    free(native);
    return wide;
}

static char *path_from_wide(const wchar_t *path)
{
    char *utf8 = wide_to_utf8(path);
    if (!utf8)
        return NULL;
    for (char *cursor = utf8; *cursor; cursor++)
        if (*cursor == '\\')
            *cursor = '/';

    if (ascii_alpha(utf8[0]) && utf8[1] == ':') {
        size_t length = strlen(utf8);
        char *canonical = malloc(length + 2);
        if (!canonical) {
            free(utf8);
            errno = ENOMEM;
            return NULL;
        }
        canonical[0] = '/';
        canonical[1] = utf8[0] >= 'A' ? (char)(utf8[0] - 'A' + 'a') : utf8[0];
        memcpy(canonical + 2, utf8 + 2, length - 1);
        free(utf8);
        return canonical;
    }
    return utf8;
}

struct console_state {
    HANDLE input;
    HANDLE output;
    DWORD input_mode;
    DWORD output_mode;
    UINT input_code_page;
    UINT output_code_page;
    int input_saved;
    int output_saved;
    int input_changed;
    int output_changed;
};

static struct console_state console_state;

int hax_isatty(int fd)
{
    return terminal_windows_stream_kind(fd) != TERMINAL_STREAM_REDIRECTED;
}

void win32_terminal_leave_for_child(void)
{
    if (console_state.input_saved) {
        SetConsoleMode(console_state.input, console_state.input_mode);
        SetConsoleCP(console_state.input_code_page);
        console_state.input_changed = 0;
    }
}

void win32_terminal_release(void)
{
    /* Restore the immutable startup state even when another cleanup path already cleared the
     * changed flags: termios teardown may subsequently have restored its saved VT mode. */
    win32_terminal_leave_for_child();
    if (console_state.output_saved) {
        SetConsoleMode(console_state.output, console_state.output_mode);
        SetConsoleOutputCP(console_state.output_code_page);
        console_state.output_changed = 0;
    }
}

static BOOL WINAPI console_control_handler(DWORD control)
{
    (void)control;
    win32_terminal_release();
    return FALSE;
}

static char *normalize_environment_path(const char *path)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return NULL;
    char *normalized = path_from_wide(wide);
    free(wide);
    return normalized;
}

static void normalize_path_environment(const char *name)
{
    const char *value = getenv(name);
    if (!value || !*value)
        return;
    char *normalized = normalize_environment_path(value);
    if (normalized) {
        _putenv_s(name, normalized);
        free(normalized);
    }
}

void win32_terminal_acquire(void)
{
    DWORD mode;
    if (!console_state.output_changed && GetConsoleMode(console_state.output, &mode)) {
        DWORD vt_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT;
        if (SetConsoleMode(console_state.output, vt_mode)) {
            SetConsoleOutputCP(CP_UTF8);
            console_state.output_changed = 1;
        }
    }
    if (!console_state.input_changed && GetConsoleMode(console_state.input, &mode)) {
        /* Preserve the caller's Quick Edit bit so selection and host scrollback keep working. */
        DWORD vt_mode = mode | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
        if (SetConsoleMode(console_state.input, vt_mode)) {
            SetConsoleCP(CP_UTF8);
            console_state.input_changed = 1;
        }
    }
}

void win32_process_init(int *argc, char ***argv)
{
    int wide_argc = 0;
    wchar_t **wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide_argv) {
        char **utf8_argv = calloc((size_t)wide_argc + 1, sizeof(*utf8_argv));
        if (utf8_argv) {
            int converted = 1;
            for (int i = 0; i < wide_argc; i++) {
                utf8_argv[i] = wide_to_utf8(wide_argv[i]);
                converted &= utf8_argv[i] != NULL;
            }
            if (converted) {
                *argc = wide_argc;
                *argv = utf8_argv;
            } else {
                for (int i = 0; i < wide_argc; i++)
                    free(utf8_argv[i]);
                free(utf8_argv);
            }
        }
        LocalFree(wide_argv);
    }

    _setmode(STDIN_FILENO, _O_BINARY);
    _setmode(STDOUT_FILENO, _O_BINARY);
    _setmode(STDERR_FILENO, _O_BINARY);

    if (!getenv("HOME") || !*getenv("HOME")) {
        const char *profile = getenv("USERPROFILE");
        char *home = profile ? normalize_environment_path(profile) : NULL;
        if (home) {
            _putenv_s("HOME", home);
            free(home);
        }
    } else {
        normalize_path_environment("HOME");
    }
    normalize_path_environment("XDG_CONFIG_HOME");
    normalize_path_environment("XDG_STATE_HOME");
    normalize_path_environment("XDG_CACHE_HOME");
    if (!getenv("TMPDIR") || !*getenv("TMPDIR")) {
        const char *temporary = getenv("TEMP");
        if (!temporary || !*temporary)
            temporary = getenv("TMP");
        char *normalized = temporary ? normalize_environment_path(temporary) : NULL;
        if (normalized) {
            _putenv_s("TMPDIR", normalized);
            free(normalized);
        }
    } else {
        normalize_path_environment("TMPDIR");
    }

    console_state.input = GetStdHandle(STD_INPUT_HANDLE);
    console_state.output = GetStdHandle(STD_OUTPUT_HANDLE);
    console_state.input_saved =
        GetConsoleMode(console_state.input, &console_state.input_mode) != FALSE;
    if (console_state.input_saved)
        console_state.input_code_page = GetConsoleCP();
    console_state.output_saved =
        GetConsoleMode(console_state.output, &console_state.output_mode) != FALSE;
    if (console_state.output_saved)
        console_state.output_code_page = GetConsoleOutputCP();
    win32_terminal_acquire();
    atexit(win32_terminal_release);
    SetConsoleCtrlHandler(console_control_handler, TRUE);
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name))
        return 0;
    return _putenv_s(name, value);
}

int unsetenv(const char *name)
{
    return _putenv_s(name, "");
}

int clock_gettime(int clock_id, struct timespec *time_value)
{
    if (!time_value) {
        errno = EINVAL;
        return -1;
    }
    if (clock_id == CLOCK_REALTIME) {
        timespec_get(time_value, TIME_UTC);
        return 0;
    }
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    time_value->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
    time_value->tv_nsec =
        (long)((counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart);
    return 0;
}

int nanosleep(const struct timespec *request, struct timespec *remaining)
{
    (void)remaining;
    if (!request || request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    uint64_t milliseconds =
        (uint64_t)request->tv_sec * 1000 + ((uint64_t)request->tv_nsec + 999999) / 1000000;
    Sleep(milliseconds > UINT32_MAX ? UINT32_MAX : (DWORD)milliseconds);
    return 0;
}

void hax_sleep_ms(long milliseconds)
{
    if (milliseconds > 0)
        Sleep((DWORD)milliseconds);
}

ssize_t hax_getline(char **line, size_t *capacity, FILE *stream)
{
    if (!line || !capacity || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*line || *capacity == 0) {
        *capacity = 128;
        *line = malloc(*capacity);
        if (!*line)
            return -1;
    }
    size_t length = 0;
    int byte;
    while ((byte = fgetc(stream)) != EOF) {
        if (length + 1 >= *capacity) {
            size_t next_capacity = *capacity > SIZE_MAX / 2 ? SIZE_MAX : *capacity * 2;
            if (next_capacity <= *capacity) {
                errno = EOVERFLOW;
                return -1;
            }
            char *next = realloc(*line, next_capacity);
            if (!next)
                return -1;
            *line = next;
            *capacity = next_capacity;
        }
        (*line)[length++] = (char)byte;
        if (byte == '\n')
            break;
    }
    if (length == 0)
        return -1;
    (*line)[length] = '\0';
    return (ssize_t)length;
}

struct tm *hax_gmtime_r(const time_t *time_value, struct tm *result)
{
    return gmtime_s(result, time_value) == 0 ? result : NULL;
}

struct tm *hax_localtime_r(const time_t *time_value, struct tm *result)
{
    return localtime_s(result, time_value) == 0 ? result : NULL;
}

struct hax_reparse_data {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    union {
        struct {
            USHORT substitute_offset;
            USHORT substitute_length;
            USHORT print_offset;
            USHORT print_length;
            ULONG flags;
            WCHAR path[1];
        } symbolic_link;
        struct {
            USHORT substitute_offset;
            USHORT substitute_length;
            USHORT print_offset;
            USHORT print_length;
            WCHAR path[1];
        } mount_point;
    } data;
};

ssize_t hax_readlink(const char *path, char *buffer, size_t capacity)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    HANDLE handle =
        CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        set_errno_from_win32(GetLastError());
        return -1;
    }

    unsigned char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    DWORD bytes_returned;
    BOOL read = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0, raw, sizeof(raw),
                                &bytes_returned, NULL);
    CloseHandle(handle);
    if (!read) {
        set_errno_from_win32(GetLastError());
        return -1;
    }

    struct hax_reparse_data *reparse = (struct hax_reparse_data *)raw;
    const wchar_t *target;
    size_t target_length;
    if (reparse->tag == IO_REPARSE_TAG_SYMLINK) {
        const USHORT offset = reparse->data.symbolic_link.print_length
                                  ? reparse->data.symbolic_link.print_offset
                                  : reparse->data.symbolic_link.substitute_offset;
        const USHORT length = reparse->data.symbolic_link.print_length
                                  ? reparse->data.symbolic_link.print_length
                                  : reparse->data.symbolic_link.substitute_length;
        target = reparse->data.symbolic_link.path + offset / sizeof(wchar_t);
        target_length = length / sizeof(wchar_t);
    } else if (reparse->tag == IO_REPARSE_TAG_MOUNT_POINT) {
        const USHORT offset = reparse->data.mount_point.print_length
                                  ? reparse->data.mount_point.print_offset
                                  : reparse->data.mount_point.substitute_offset;
        const USHORT length = reparse->data.mount_point.print_length
                                  ? reparse->data.mount_point.print_length
                                  : reparse->data.mount_point.substitute_length;
        target = reparse->data.mount_point.path + offset / sizeof(wchar_t);
        target_length = length / sizeof(wchar_t);
    } else {
        errno = EINVAL;
        return -1;
    }

    wchar_t *terminated = malloc((target_length + 1) * sizeof(*terminated));
    if (!terminated) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(terminated, target, target_length * sizeof(*terminated));
    terminated[target_length] = L'\0';
    wchar_t *normalized = terminated;
    if (wcsncmp(normalized, L"\\??\\UNC\\", 8) == 0) {
        normalized[6] = L'\\';
        normalized += 6;
    } else if (wcsncmp(normalized, L"\\??\\", 4) == 0) {
        normalized += 4;
    }
    char *canonical = path_from_wide(normalized);
    free(terminated);
    if (!canonical)
        return -1;
    size_t bytes = strlen(canonical);
    if (bytes > capacity) {
        free(canonical);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, canonical, bytes);
    free(canonical);
    return (ssize_t)bytes;
}

static FILETIME timespec_to_filetime(const struct timespec *time_value)
{
    uint64_t ticks = ((uint64_t)time_value->tv_sec + 11644473600ULL) * 10000000ULL +
                     (uint64_t)time_value->tv_nsec / 100;
    FILETIME file_time = {.dwLowDateTime = (DWORD)ticks, .dwHighDateTime = (DWORD)(ticks >> 32)};
    return file_time;
}

int futimens(int fd, const struct timespec times[2])
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    FILETIME access_time;
    FILETIME write_time;
    FILETIME *access_ptr = NULL;
    FILETIME *write_ptr = NULL;
    if (!times) {
        GetSystemTimeAsFileTime(&access_time);
        write_time = access_time;
        access_ptr = &access_time;
        write_ptr = &write_time;
    } else {
        if (times[0].tv_nsec != UTIME_OMIT) {
            access_time = timespec_to_filetime(&times[0]);
            access_ptr = &access_time;
        }
        if (times[1].tv_nsec != UTIME_OMIT) {
            write_time = timespec_to_filetime(&times[1]);
            write_ptr = &write_time;
        }
    }
    if (!SetFileTime(handle, NULL, access_ptr, write_ptr)) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    return 0;
}

int hax_utimensat(int directory_fd, const char *path, const struct timespec times[2], int flags)
{
    (void)flags;
    if (directory_fd != AT_FDCWD) {
        errno = ENOTSUP;
        return -1;
    }
    int fd = hax_open(path, O_RDWR);
    if (fd < 0)
        return -1;
    int result = futimens(fd, times);
    _close(fd);
    return result;
}

int hax_pipe(int fds[2])
{
    return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT);
}

int hax_fcntl(int fd, int command, ...)
{
    if (command == F_GETFL)
        return 0;
    if (command == F_SETFD || command == F_SETFL)
        return 0;
    (void)fd;
    errno = EINVAL;
    return -1;
}

int hax_wcwidth(wchar_t codepoint)
{
    if (codepoint == 0)
        return 0;
    if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint < 0xa0))
        return -1;
    WORD type = 0;
    if (GetStringTypeW(CT_CTYPE3, &codepoint, 1, &type) &&
        (type & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)))
        return 0;
    if ((codepoint >= 0x1100 && codepoint <= 0x115f) ||
        (codepoint >= 0x2e80 && codepoint <= 0xa4cf) ||
        (codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
        (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
        (codepoint >= 0xfe10 && codepoint <= 0xfe6f) ||
        (codepoint >= 0xff00 && codepoint <= 0xff60) ||
        (codepoint >= 0xffe0 && codepoint <= 0xffe6))
        return 2;
    return 1;
}

int hax_open(const char *path, int flags, ...)
{
    int mode = _S_IREAD | _S_IWRITE;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    int fd = _wopen(wide, flags | _O_BINARY | _O_NOINHERIT, mode);
    free(wide);
    return fd;
}

static char *binary_mode(const char *mode)
{
    return strchr(mode, 'b') ? _strdup(mode) : xasprintf("%sb", mode);
}

FILE *hax_fopen(const char *path, const char *mode)
{
    wchar_t *wide_path = path_to_wide(path);
    char *binary = binary_mode(mode);
    wchar_t *wide_mode = binary ? utf8_to_wide(binary) : NULL;
    free(binary);
    if (!wide_path || !wide_mode) {
        free(wide_path);
        free(wide_mode);
        return NULL;
    }
    FILE *file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    free(wide_mode);
    return file;
}

FILE *hax_freopen(const char *path, const char *mode, FILE *stream)
{
    wchar_t *wide_path = path_to_wide(path);
    char *binary = binary_mode(mode);
    wchar_t *wide_mode = binary ? utf8_to_wide(binary) : NULL;
    free(binary);
    if (!wide_path || !wide_mode) {
        free(wide_path);
        free(wide_mode);
        return NULL;
    }
    FILE *file = _wfreopen(wide_path, wide_mode, stream);
    free(wide_path);
    free(wide_mode);
    return file;
}

struct memory_stream {
    FILE *stream;
    char **buffer;
    size_t *length;
    struct memory_stream *next;
};

static SRWLOCK memory_stream_lock = SRWLOCK_INIT;
static struct memory_stream *memory_streams;

FILE *hax_open_memstream(char **buffer, size_t *length)
{
    if (!buffer || !length) {
        errno = EINVAL;
        return NULL;
    }
    FILE *stream = tmpfile();
    if (!stream)
        return NULL;
    struct memory_stream *entry = malloc(sizeof(*entry));
    if (!entry) {
        fclose(stream);
        errno = ENOMEM;
        return NULL;
    }
    *buffer = NULL;
    *length = 0;
    *entry = (struct memory_stream){.stream = stream, .buffer = buffer, .length = length};
    AcquireSRWLockExclusive(&memory_stream_lock);
    entry->next = memory_streams;
    memory_streams = entry;
    ReleaseSRWLockExclusive(&memory_stream_lock);
    return stream;
}

int hax_fflush(FILE *stream)
{
    int result = fflush(stream);
    if (!stream)
        return result;

    AcquireSRWLockExclusive(&memory_stream_lock);
    struct memory_stream *entry = memory_streams;
    while (entry && entry->stream != stream)
        entry = entry->next;
    if (entry && result == 0) {
        __int64 position = _ftelli64(stream);
        if (position >= 0 && _fseeki64(stream, 0, SEEK_SET) == 0) {
            char *data = malloc((size_t)position + 1);
            if (data) {
                size_t bytes_read = fread(data, 1, (size_t)position, stream);
                data[bytes_read] = '\0';
                free(*entry->buffer);
                *entry->buffer = data;
                *entry->length = bytes_read;
                if (_fseeki64(stream, position, SEEK_SET) != 0)
                    result = -1;
            } else {
                errno = ENOMEM;
                result = -1;
            }
        } else {
            result = -1;
        }
    }
    ReleaseSRWLockExclusive(&memory_stream_lock);
    return result;
}

int hax_fclose(FILE *stream)
{
    int result = hax_fflush(stream);
    AcquireSRWLockExclusive(&memory_stream_lock);
    struct memory_stream **link = &memory_streams;
    while (*link && (*link)->stream != stream)
        link = &(*link)->next;
    struct memory_stream *entry = *link;
    if (entry)
        *link = entry->next;
    ReleaseSRWLockExclusive(&memory_stream_lock);
    if (fclose(stream) != 0)
        result = -1;
    free(entry);
    return result;
}

int hax_access(const char *path, int mode)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    int result = _waccess(wide, mode == X_OK ? F_OK : mode);
    free(wide);
    return result;
}

int hax_chdir(const char *path)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    int result = _wchdir(wide);
    free(wide);
    return result;
}

int hax_chmod(const char *path, int mode)
{
    (void)mode;
    return hax_access(path, F_OK);
}

char *hax_getcwd(char *buffer, int size)
{
    wchar_t *wide = _wgetcwd(NULL, 0);
    if (!wide)
        return NULL;
    char *canonical = path_from_wide(wide);
    free(wide);
    if (!canonical)
        return NULL;
    size_t needed = strlen(canonical) + 1;
    if (!buffer) {
        if (size > 0 && needed > (size_t)size) {
            free(canonical);
            errno = ERANGE;
            return NULL;
        }
        return canonical;
    }
    if (size <= 0 || needed > (size_t)size) {
        free(canonical);
        errno = ERANGE;
        return NULL;
    }
    memcpy(buffer, canonical, needed);
    free(canonical);
    return buffer;
}

int hax_mkdir(const char *path)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    int result = _wmkdir(wide);
    free(wide);
    return result;
}

int hax_rename(const char *old_path, const char *new_path)
{
    wchar_t *wide_old = path_to_wide(old_path);
    wchar_t *wide_new = path_to_wide(new_path);
    if (!wide_old || !wide_new) {
        free(wide_old);
        free(wide_new);
        return -1;
    }
    DWORD destination_attributes = GetFileAttributesW(wide_new);
    BOOL moved = destination_attributes != INVALID_FILE_ATTRIBUTES
                     ? ReplaceFileW(wide_new, wide_old, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL)
                     : MoveFileExW(wide_old, wide_new, MOVEFILE_WRITE_THROUGH);
    if (!moved) {
        moved = MoveFileExW(wide_old, wide_new, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!moved)
            set_errno_from_win32(GetLastError());
    }
    free(wide_old);
    free(wide_new);
    return moved ? 0 : -1;
}

static int path_delete(const char *path, int directory)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    BOOL removed = directory ? RemoveDirectoryW(wide) : DeleteFileW(wide);
    if (!removed)
        set_errno_from_win32(GetLastError());
    free(wide);
    return removed ? 0 : -1;
}

int hax_remove(const char *path)
{
    if (hax_unlink(path) == 0)
        return 0;
    return hax_rmdir(path);
}

int hax_rmdir(const char *path)
{
    return path_delete(path, 1);
}

int hax_unlink(const char *path)
{
    return path_delete(path, 0);
}

int hax_stat(const char *path, struct stat *status)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    struct _stat64 native;
    int result = _wstat64(wide, &native);
    free(wide);
    if (result != 0)
        return result;
    memset(status, 0, sizeof(*status));
    status->st_dev = native.st_dev;
    status->st_ino = native.st_ino;
    status->st_mode = native.st_mode;
    status->st_nlink = native.st_nlink;
    status->st_uid = native.st_uid;
    status->st_gid = native.st_gid;
    status->st_rdev = native.st_rdev;
    status->st_size = native.st_size;
    status->st_atime = native.st_atime;
    status->st_mtime = native.st_mtime;
    status->st_ctime = native.st_ctime;
    return 0;
}

int hax_lstat(const char *path, struct stat *status)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return -1;
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFLNK;
        char target[32768];
        ssize_t target_length = hax_readlink(path, target, sizeof(target));
        if (target_length < 0)
            return -1;
        status->st_size = target_length;
        return 0;
    }
    return hax_stat(path, status);
}

int hax_symlink(const char *target, const char *link_path)
{
    wchar_t *wide_target = path_to_wide(target);
    wchar_t *wide_link = path_to_wide(link_path);
    if (!wide_target || !wide_link) {
        free(wide_target);
        free(wide_link);
        return -1;
    }
    DWORD attributes = GetFileAttributesW(wide_target);
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (attributes != INVALID_FILE_ATTRIBUTES && attributes & FILE_ATTRIBUTE_DIRECTORY)
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    BOOLEAN created = CreateSymbolicLinkW(wide_link, wide_target, flags);
    if (!created)
        set_errno_from_win32(GetLastError());
    free(wide_target);
    free(wide_link);
    return created ? 0 : -1;
}

char *hax_realpath(const char *path, char *resolved)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return NULL;
    DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
    if (!needed) {
        set_errno_from_win32(GetLastError());
        free(wide);
        return NULL;
    }
    wchar_t *full = malloc((size_t)needed * sizeof(*full));
    if (!full) {
        free(wide);
        errno = ENOMEM;
        return NULL;
    }
    if (!GetFullPathNameW(wide, needed, full, NULL)) {
        set_errno_from_win32(GetLastError());
        free(full);
        free(wide);
        return NULL;
    }
    free(wide);
    char *canonical = path_from_wide(full);
    free(full);
    if (!canonical)
        return NULL;
    if (!resolved)
        return canonical;
    strcpy(resolved, canonical);
    free(canonical);
    return resolved;
}

char *hax_search_path(const char *name)
{
    wchar_t *wide_name = utf8_to_wide(name);
    if (!wide_name)
        return NULL;
    DWORD length = SearchPathW(NULL, wide_name, NULL, 0, NULL, NULL);
    if (!length) {
        free(wide_name);
        return NULL;
    }
    wchar_t *wide_path = malloc(((size_t)length + 1) * sizeof(*wide_path));
    if (!wide_path) {
        free(wide_name);
        return NULL;
    }
    if (!SearchPathW(NULL, wide_name, NULL, length + 1, wide_path, NULL)) {
        free(wide_path);
        free(wide_name);
        return NULL;
    }
    free(wide_name);
    char *path = path_from_wide(wide_path);
    free(wide_path);
    return path;
}

static int fill_random(void *data, size_t length)
{
    return BCryptGenRandom(NULL, data, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0
                                                                                            : -1;
}

int hax_mkstemp(char *path_template)
{
    size_t length = path_template ? strlen(path_template) : 0;
    if (length < 6 || strcmp(path_template + length - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 128; attempt++) {
        unsigned char random[6];
        if (fill_random(random, sizeof(random)) != 0) {
            errno = EIO;
            return -1;
        }
        for (size_t i = 0; i < sizeof(random); i++)
            path_template[length - 6 + i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
        int fd = hax_open(path_template, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

char *hax_mkdtemp(char *path_template)
{
    size_t length = path_template ? strlen(path_template) : 0;
    if (length < 6 || strcmp(path_template + length - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return NULL;
    }
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 128; attempt++) {
        unsigned char random[6];
        if (fill_random(random, sizeof(random)) != 0) {
            errno = EIO;
            return NULL;
        }
        for (size_t i = 0; i < sizeof(random); i++)
            path_template[length - 6 + i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
        if (hax_mkdir(path_template) == 0)
            return path_template;
        if (errno != EEXIST)
            return NULL;
    }
    errno = EEXIST;
    return NULL;
}

int hax_pread(int fd, void *buffer, size_t count, off_t offset)
{
    __int64 original = _telli64(fd);
    if (original < 0 || _lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;
    int result = _read(fd, buffer, count > INT_MAX ? INT_MAX : (unsigned int)count);
    (void)_lseeki64(fd, original, SEEK_SET);
    return result;
}

int flock(int fd, int operation)
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    /* Lock a sentinel byte beyond any valid session rather than the data range: Windows byte-range
     * locks, unlike flock(2), also block writes through the locking handle. */
    OVERLAPPED overlapped = {.Offset = UINT32_MAX};
    if (operation & LOCK_UN) {
        if (UnlockFileEx(handle, 0, 1, 0, &overlapped))
            return 0;
    } else {
        DWORD flags = operation & LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0;
        if (operation & LOCK_NB)
            flags |= LOCKFILE_FAIL_IMMEDIATELY;
        if (LockFileEx(handle, flags, 0, 1, 0, &overlapped))
            return 0;
    }
    set_errno_from_win32(GetLastError());
    return -1;
}

struct hax_win32_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    int first;
    struct dirent entry;
};

DIR *opendir(const char *path)
{
    wchar_t *wide = path_to_wide(path);
    if (!wide)
        return NULL;
    size_t length = wcslen(wide);
    wchar_t *pattern = malloc((length + 3) * sizeof(*pattern));
    if (!pattern) {
        free(wide);
        errno = ENOMEM;
        return NULL;
    }
    wcscpy(pattern, wide);
    if (length && pattern[length - 1] != L'\\')
        pattern[length++] = L'\\';
    pattern[length++] = L'*';
    pattern[length] = L'\0';
    free(wide);

    DIR *directory = calloc(1, sizeof(*directory));
    if (!directory) {
        free(pattern);
        errno = ENOMEM;
        return NULL;
    }
    directory->find = FindFirstFileW(pattern, &directory->data);
    free(pattern);
    if (directory->find == INVALID_HANDLE_VALUE) {
        set_errno_from_win32(GetLastError());
        free(directory);
        return NULL;
    }
    directory->first = 1;
    return directory;
}

struct dirent *readdir(DIR *directory)
{
    if (!directory) {
        errno = EBADF;
        return NULL;
    }
    for (;;) {
        if (directory->first)
            directory->first = 0;
        else if (!FindNextFileW(directory->find, &directory->data)) {
            if (GetLastError() != ERROR_NO_MORE_FILES)
                set_errno_from_win32(GetLastError());
            return NULL;
        }
        char *name = wide_to_utf8(directory->data.cFileName);
        if (!name)
            return NULL;
        size_t length = strlen(name);
        if (length >= sizeof(directory->entry.d_name)) {
            free(name);
            errno = ENAMETOOLONG;
            return NULL;
        }
        memcpy(directory->entry.d_name, name, length + 1);
        free(name);
        directory->entry.d_ino = 0;
        DWORD attributes = directory->data.dwFileAttributes;
        directory->entry.d_type = attributes & FILE_ATTRIBUTE_REPARSE_POINT ? DT_LNK
                                  : attributes & FILE_ATTRIBUTE_DIRECTORY   ? DT_DIR
                                                                            : DT_REG;
        return &directory->entry;
    }
}

int closedir(DIR *directory)
{
    if (!directory)
        return -1;
    FindClose(directory->find);
    free(directory);
    return 0;
}

int dirfd(DIR *directory)
{
    (void)directory;
    errno = ENOTSUP;
    return -1;
}

DIR *fdopendir(int fd)
{
    (void)fd;
    errno = ENOTSUP;
    return NULL;
}

char *basename(char *path)
{
    if (!path || !*path)
        return ".";
    char *last = path + strlen(path) - 1;
    while (last > path && (*last == '/' || *last == '\\'))
        *last-- = '\0';
    char *slash = strrchr(path, '/');
    char *backslash = strrchr(path, '\\');
    char *separator = slash > backslash ? slash : backslash;
    return separator ? separator + 1 : path;
}

char *dirname(char *path)
{
    if (!path || !*path)
        return ".";
    char *last = path + strlen(path) - 1;
    while (last > path && (*last == '/' || *last == '\\'))
        *last-- = '\0';
    char *slash = strrchr(path, '/');
    char *backslash = strrchr(path, '\\');
    char *separator = slash > backslash ? slash : backslash;
    if (!separator)
        return ".";
    if (separator == path) {
        path[1] = '\0';
        return path;
    }
    *separator = '\0';
    return path;
}

static int glob_match(const char *pattern, const char *text)
{
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*')
                pattern++;
            if (!*pattern)
                return 1;
            while (*text)
                if (glob_match(pattern, text++))
                    return 1;
            return 0;
        }
        if (*pattern == '?') {
            if (!*text)
                return 0;
            pattern++;
            text++;
            continue;
        }
        if (*pattern != *text)
            return 0;
        pattern++;
        text++;
    }
    return *text == '\0';
}

int fnmatch(const char *pattern, const char *text, int flags)
{
    (void)flags;
    return glob_match(pattern, text) ? 0 : 1;
}

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

int getopt_long(int argc, char *const argv[], const char *short_options,
                const struct option *long_options, int *long_index)
{
    (void)short_options;
    optarg = NULL;
    if (optind >= argc)
        return -1;
    char *argument = argv[optind];
    if (argument[0] != '-' || argument[1] == '\0')
        return -1;
    if (strcmp(argument, "--") == 0) {
        optind++;
        return -1;
    }
    if (argument[1] != '-') {
        optind++;
        if (argument[2] != '\0') {
            optopt = argument[1];
            return '?';
        }
        return (unsigned char)argument[1];
    }

    const char *name = argument + 2;
    const char *equals = strchr(name, '=');
    size_t name_length = equals ? (size_t)(equals - name) : strlen(name);
    for (int i = 0; long_options[i].name; i++) {
        if (strlen(long_options[i].name) != name_length ||
            strncmp(long_options[i].name, name, name_length) != 0)
            continue;
        optind++;
        if (long_index)
            *long_index = i;
        if (equals) {
            if (long_options[i].has_arg == no_argument)
                return '?';
            optarg = (char *)equals + 1;
        } else if (long_options[i].has_arg == required_argument) {
            if (optind >= argc)
                return '?';
            optarg = argv[optind++];
        }
        if (long_options[i].flag) {
            *long_options[i].flag = long_options[i].val;
            return 0;
        }
        return long_options[i].val;
    }
    optind++;
    return '?';
}

int tcgetattr(int fd, struct termios *attributes)
{
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    DWORD mode;
    if (handle == INVALID_HANDLE_VALUE) {
        errno = ENOTTY;
        return -1;
    }
    if (!GetConsoleMode(handle, &mode)) {
        if (terminal_windows_stream_kind(fd) == TERMINAL_STREAM_MSYS_PTY) {
            memset(attributes, 0, sizeof(*attributes));
            attributes->c_lflag = ICANON | ECHO | ISIG;
            return 0;
        }
        errno = ENOTTY;
        return -1;
    }
    memset(attributes, 0, sizeof(*attributes));
    attributes->input_mode = mode;
    if (mode & ENABLE_LINE_INPUT)
        attributes->c_lflag |= ICANON;
    if (mode & ENABLE_ECHO_INPUT)
        attributes->c_lflag |= ECHO;
    if (mode & ENABLE_PROCESSED_INPUT)
        attributes->c_lflag |= ISIG;
    return 0;
}

int tcsetattr(int fd, int action, const struct termios *attributes)
{
    (void)action;
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle != INVALID_HANDLE_VALUE &&
        terminal_windows_stream_kind(fd) == TERMINAL_STREAM_MSYS_PTY)
        return 0;
    DWORD mode = attributes->input_mode | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS;
    if (attributes->c_lflag & ICANON)
        mode |= ENABLE_LINE_INPUT;
    else
        mode &= ~ENABLE_LINE_INPUT;
    if (attributes->c_lflag & ECHO)
        mode |= ENABLE_ECHO_INPUT;
    else
        mode &= ~ENABLE_ECHO_INPUT;
    if (attributes->c_lflag & ISIG)
        mode |= ENABLE_PROCESSED_INPUT;
    else
        mode &= ~ENABLE_PROCESSED_INPUT;
    if (handle == INVALID_HANDLE_VALUE || !SetConsoleMode(handle, mode)) {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

int tcflush(int fd, int queue)
{
    (void)queue;
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    return handle != INVALID_HANDLE_VALUE && FlushConsoleInputBuffer(handle) ? 0 : -1;
}

int ioctl(int fd, int request, ...)
{
    if (request != TIOCGWINSZ) {
        errno = EINVAL;
        return -1;
    }
    va_list args;
    va_start(args, request);
    struct winsize *size = va_arg(args, struct winsize *);
    va_end(args);
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(handle, &info)) {
        errno = ENOTTY;
        return -1;
    }
    size->ws_col = (unsigned short)(info.srWindow.Right - info.srWindow.Left + 1);
    size->ws_row = (unsigned short)(info.srWindow.Bottom - info.srWindow.Top + 1);
    return 0;
}

static int console_input_ready(HANDLE handle)
{
    for (int i = 0; i < 64; i++) {
        INPUT_RECORD record;
        DWORD count;
        if (!PeekConsoleInputW(handle, &record, 1, &count) || count == 0)
            return 0;
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
            WORD key = record.Event.KeyEvent.wVirtualKeyCode;
            if (record.Event.KeyEvent.uChar.UnicodeChar != L'\0' && key != VK_SHIFT &&
                key != VK_CONTROL && key != VK_MENU && key != VK_CAPITAL && key != VK_NUMLOCK &&
                key != VK_SCROLL)
                return 1;
        }
        if (!ReadConsoleInputW(handle, &record, 1, &count))
            return 0;
    }
    return 0;
}

int poll(struct pollfd *fds, nfds_t count, int timeout_ms)
{
    if (!fds || count == 0) {
        if (timeout_ms > 0)
            Sleep((DWORD)timeout_ms);
        return 0;
    }
    DWORD started = GetTickCount();
    for (;;) {
        int ready = 0;
        for (nfds_t i = 0; i < count; i++) {
            fds[i].revents = 0;
            HANDLE handle = (HANDLE)_get_osfhandle(fds[i].fd);
            if (handle == INVALID_HANDLE_VALUE) {
                fds[i].revents = POLLNVAL;
            } else if (fds[i].events & POLLIN) {
                DWORD type = GetFileType(handle);
                if (type == FILE_TYPE_PIPE) {
                    DWORD available = 0;
                    if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL))
                        fds[i].revents = POLLHUP;
                    else if (available)
                        fds[i].revents = POLLIN;
                } else {
                    DWORD console_mode;
                    if (GetConsoleMode(handle, &console_mode)) {
                        if (console_input_ready(handle))
                            fds[i].revents = POLLIN;
                    } else if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) {
                        fds[i].revents = POLLIN;
                    }
                }
            } else if (fds[i].events & POLLOUT) {
                fds[i].revents = POLLOUT;
            }
            ready += fds[i].revents != 0;
        }
        if (ready || timeout_ms == 0)
            return ready;
        if (timeout_ms > 0 && GetTickCount() - started >= (DWORD)timeout_ms)
            return 0;
        Sleep(1);
    }
}

int hax_kill(pid_t pid, int signal_number)
{
    if (pid < 0)
        pid = -pid;
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!process) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    BOOL terminated = TerminateProcess(process, signal_number ? 128 + signal_number : 1);
    if (!terminated)
        set_errno_from_win32(GetLastError());
    CloseHandle(process);
    return terminated ? 0 : -1;
}

pid_t spawn_win32_waitpid(pid_t pid, int *status, int options);

pid_t waitpid(pid_t pid, int *status, int options)
{
    pid_t registered = spawn_win32_waitpid(pid, status, options);
    if (registered >= 0 || errno != ECHILD)
        return registered;
    HANDLE process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!process) {
        errno = ECHILD;
        return -1;
    }
    DWORD wait = WaitForSingleObject(process, options & WNOHANG ? 0 : INFINITE);
    if (wait == WAIT_TIMEOUT) {
        CloseHandle(process);
        return 0;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process, &exit_code);
    CloseHandle(process);
    if (status)
        *status = exit_code < 256 ? (int)exit_code : 1;
    return pid;
}

int uname(struct utsname *name)
{
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    memset(name, 0, sizeof(*name));
    strcpy(name->sysname, "Windows");
#if defined(_M_X64)
    strcpy(name->machine, "x86_64");
#elif defined(_M_ARM64)
    strcpy(name->machine, "arm64");
#else
    strcpy(name->machine, "unknown");
#endif
    typedef LONG(WINAPI * rtl_get_version_fn)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    rtl_get_version_fn get_version =
        ntdll ? (rtl_get_version_fn)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW version = {.dwOSVersionInfoSize = sizeof(version)};
    if (get_version && get_version(&version) == 0)
        snprintf(name->release, sizeof(name->release), "%lu.%lu.%lu", version.dwMajorVersion,
                 version.dwMinorVersion, version.dwBuildNumber);
    else
        strcpy(name->release, "10+");
    return 0;
}

#else

typedef int hax_win32_empty_translation_unit;

#endif /* _WIN32 */
