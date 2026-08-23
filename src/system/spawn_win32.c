/* SPDX-License-Identifier: MIT */
#ifdef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <sys/wait.h>

#include "util.h"
#include "system/path.h"
#include "system/spawn.h"

struct child_record {
    pid_t pid;
    HANDLE process;
    HANDLE job;
    struct child_record *next;
};

static SRWLOCK child_lock = SRWLOCK_INIT;
static struct child_record *children;

static wchar_t *utf8_to_wide(const char *text)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0)
        return NULL;
    wchar_t *wide = xmalloc((size_t)length * sizeof(*wide));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static wchar_t *internal_path_to_wide(const char *path)
{
    if (!path)
        return NULL;
    char *native;
    if (path[0] == '/' && path[1] && path[2] == '/') {
        native = xasprintf("%c:%s", path[1] >= 'a' ? path[1] - 'a' + 'A' : path[1], path + 2);
    } else if (path[0] == '/' && path[1] == '/') {
        native = xasprintf("\\\\%s", path + 2);
    } else {
        native = xstrdup(path);
    }
    for (char *cursor = native; *cursor; cursor++)
        if (*cursor == '/')
            *cursor = '\\';
    wchar_t *wide = utf8_to_wide(native);
    free(native);
    return wide;
}

static void append_quoted_arg(struct buf *command, const char *argument)
{
    int quote = !*argument || strpbrk(argument, " \t\n\v\"") != NULL;
    if (!quote) {
        buf_append_str(command, argument);
        return;
    }
    buf_append_str(command, "\"");
    size_t backslashes = 0;
    for (const char *cursor = argument;; cursor++) {
        if (*cursor == '\\') {
            backslashes++;
            continue;
        }
        if (*cursor == '\"' || *cursor == '\0') {
            for (size_t i = 0; i < backslashes * 2 + (*cursor == '\"'); i++)
                buf_append_str(command, "\\");
        } else {
            for (size_t i = 0; i < backslashes; i++)
                buf_append_str(command, "\\");
        }
        backslashes = 0;
        if (*cursor == '\0')
            break;
        buf_append(command, cursor, 1);
    }
    buf_append_str(command, "\"");
}

static wchar_t *argv_command_line(const char *const *argv)
{
    struct buf command;
    buf_init(&command);
    for (size_t i = 0; argv[i]; i++) {
        if (i)
            buf_append_str(&command, " ");
        append_quoted_arg(&command, argv[i]);
    }
    char *utf8 = buf_steal(&command);
    wchar_t *wide = utf8_to_wide(utf8);
    free(utf8);
    return wide;
}

static wchar_t *search_executable(const char *name)
{
    wchar_t *wide_name = internal_path_to_wide(name);
    if (!wide_name)
        return NULL;
    if (wcschr(wide_name, L'\\') || wcschr(wide_name, L'/')) {
        DWORD attributes = GetFileAttributesW(wide_name);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            return wide_name;
        free(wide_name);
        return NULL;
    }
    DWORD length = SearchPathW(NULL, wide_name, NULL, 0, NULL, NULL);
    if (!length) {
        free(wide_name);
        return NULL;
    }
    wchar_t *path = xmalloc(((size_t)length + 1) * sizeof(*path));
    if (!SearchPathW(NULL, wide_name, NULL, length + 1, path, NULL)) {
        free(path);
        path = NULL;
    }
    free(wide_name);
    return path;
}

static int regular_file(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static wchar_t *find_git_bash(void)
{
    const char *configured = getenv("HAX_BASH_SHELL");
    if (configured && *configured)
        return search_executable(configured);

    static const char *const GIT_CANDIDATES[] = {"git.exe", "git.cmd", "git.bat", "git"};
    wchar_t *git = NULL;
    for (size_t i = 0; !git && i < sizeof(GIT_CANDIDATES) / sizeof(*GIT_CANDIDATES); i++)
        git = search_executable(GIT_CANDIDATES[i]);
    if (!git)
        return NULL;
    wchar_t *slash = wcsrchr(git, L'\\');
    if (slash)
        *slash = L'\0';

    for (;;) {
        static const wchar_t *const BASH_LOCATIONS[] = {L"usr\\bin\\bash.exe", L"bin\\bash.exe"};
        for (size_t i = 0; i < sizeof(BASH_LOCATIONS) / sizeof(*BASH_LOCATIONS); i++) {
            size_t length = wcslen(git) + 1 + wcslen(BASH_LOCATIONS[i]) + 1;
            wchar_t *candidate = xmalloc(length * sizeof(*candidate));
            _snwprintf(candidate, length, L"%ls\\%ls", git, BASH_LOCATIONS[i]);
            if (regular_file(candidate)) {
                free(git);
                return candidate;
            }
            free(candidate);
        }
        slash = wcsrchr(git, L'\\');
        if (!slash || slash == git + 2)
            break;
        *slash = L'\0';
    }
    free(git);
    return NULL;
}

struct child_start {
    PROCESS_INFORMATION process_info;
    HANDLE job;
};

static int start_process(const wchar_t *application, wchar_t *command_line, HANDLE stdin_handle,
                         HANDLE stdout_handle, HANDLE stderr_handle, void *environment,
                         struct child_start *out)
{
    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_handle;
    startup.StartupInfo.hStdOutput = stdout_handle;
    startup.StartupInfo.hStdError = stderr_handle;

    HANDLE inherited[3];
    size_t inherited_count = 0;
    const HANDLE candidates[] = {stdin_handle, stdout_handle, stderr_handle};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
        int duplicate = 0;
        for (size_t j = 0; j < inherited_count; j++)
            duplicate |= inherited[j] == candidates[i];
        if (!duplicate && candidates[i] && candidates[i] != INVALID_HANDLE_VALUE)
            inherited[inherited_count++] = candidates[i];
    }
    SIZE_T attributes_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    startup.lpAttributeList = xmalloc(attributes_size);
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, inherited_count * sizeof(*inherited), NULL, NULL)) {
        free(startup.lpAttributeList);
        return -1;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
        return -1;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
        return -1;
    }

    PROCESS_INFORMATION process_info;
    win32_terminal_leave_for_child();
    BOOL created =
        CreateProcessW(application, command_line, NULL, NULL, TRUE,
                       CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                       environment, NULL, &startup.StartupInfo, &process_info);
    win32_terminal_acquire();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    if (!created) {
        CloseHandle(job);
        return -1;
    }
    if (!AssignProcessToJobObject(job, process_info.hProcess)) {
        TerminateProcess(process_info.hProcess, 1);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        CloseHandle(job);
        return -1;
    }
    ResumeThread(process_info.hThread);
    CloseHandle(process_info.hThread);
    *out = (struct child_start){.process_info = process_info, .job = job};
    return 0;
}

static int child_finish(struct child_start *child, DWORD timeout_ms, int terminate_on_timeout)
{
    DWORD wait = WaitForSingleObject(child->process_info.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT && terminate_on_timeout) {
        TerminateJobObject(child->job, 1);
        wait = WaitForSingleObject(child->process_info.hProcess, INFINITE);
    }
    DWORD exit_code = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(child->process_info.hProcess, &exit_code);
    CloseHandle(child->process_info.hProcess);
    CloseHandle(child->job);
    return wait == WAIT_OBJECT_0 ? (int)(exit_code & 0xff) : -1;
}

static void child_publish(struct child_start *child)
{
    struct child_record *record = xmalloc(sizeof(*record));
    record->pid = (pid_t)child->process_info.dwProcessId;
    record->process = child->process_info.hProcess;
    record->job = child->job;
    AcquireSRWLockExclusive(&child_lock);
    record->next = children;
    children = record;
    ReleaseSRWLockExclusive(&child_lock);
}

static struct child_record *child_take(pid_t pid)
{
    AcquireSRWLockExclusive(&child_lock);
    struct child_record **link = &children;
    while (*link && (*link)->pid != pid)
        link = &(*link)->next;
    struct child_record *record = *link;
    if (record)
        *link = record->next;
    ReleaseSRWLockExclusive(&child_lock);
    return record;
}

static int compare_env(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return _stricmp(*a, *b);
}

static wchar_t *environment_block(char *const envp[])
{
    size_t count = 0;
    while (envp && envp[count])
        count++;
    if (!count)
        return NULL;
    qsort((void *)envp, count, sizeof(*envp), compare_env);

    size_t capacity = 1024;
    size_t length = 0;
    wchar_t *block = xmalloc(capacity * sizeof(*block));
    for (size_t i = 0; i < count; i++) {
        int item_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, envp[i], -1, NULL, 0);
        if (item_length <= 0) {
            free(block);
            return NULL;
        }
        if (length + (size_t)item_length + 1 > capacity) {
            while (length + (size_t)item_length + 1 > capacity)
                capacity *= 2;
            block = xrealloc(block, capacity * sizeof(*block));
        }
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, envp[i], -1, block + length,
                            item_length);
        length += (size_t)item_length;
    }
    block[length] = L'\0';
    return block;
}

static wchar_t *bash_command_line(const wchar_t *bash, const char *shell_cmd)
{
    char *bash_utf8 = NULL;
    int length = WideCharToMultiByte(CP_UTF8, 0, bash, -1, NULL, 0, NULL, NULL);
    if (length > 0) {
        bash_utf8 = xmalloc((size_t)length);
        WideCharToMultiByte(CP_UTF8, 0, bash, -1, bash_utf8, length, NULL, NULL);
    }
    if (!bash_utf8)
        return NULL;
    const char *argv[] = {bash_utf8, "--noprofile", "--norc", "-c", shell_cmd, NULL};
    wchar_t *command_line = argv_command_line(argv);
    free(bash_utf8);
    return command_line;
}

char *spawn_win32_bash_path(void)
{
    wchar_t *bash = find_git_bash();
    if (!bash)
        return NULL;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, bash, -1, NULL, 0, NULL, NULL);
    char *native = length > 0 ? xmalloc((size_t)length) : NULL;
    if (native)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, bash, -1, native, length, NULL, NULL);
    free(bash);
    if (!native)
        return NULL;
    char *canonical = path_normalize_windows(native);
    free(native);
    return canonical;
}

int spawn_win32_bash_available(void)
{
    wchar_t *bash = find_git_bash();
    if (!bash)
        return 0;
    wchar_t *command_line =
        bash_command_line(bash, "test -n \"$BASH_VERSION\" && cd /c && test \"$(pwd -W)\" = C:/");
    struct child_start child;
    int started = command_line ? start_process(bash, command_line, GetStdHandle(STD_INPUT_HANDLE),
                                               GetStdHandle(STD_OUTPUT_HANDLE),
                                               GetStdHandle(STD_ERROR_HANDLE), NULL, &child)
                               : -1;
    free(command_line);
    free(bash);
    return started == 0 && child_finish(&child, 3000, 1) == 0;
}

int spawn_win32_start_bash(const char *command, char *const envp[], pid_t *pid, int *output_fd)
{
    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE read_handle;
    HANDLE write_handle;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0))
        return -1;
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_handle =
        CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &security, OPEN_EXISTING, 0, NULL);
    wchar_t *bash = find_git_bash();
    wchar_t *command_line = bash ? bash_command_line(bash, command) : NULL;
    wchar_t *environment = environment_block(envp);
    struct child_start child;
    int started = bash && command_line && environment
                      ? start_process(bash, command_line, null_handle, write_handle, write_handle,
                                      environment, &child)
                      : -1;
    free(environment);
    free(command_line);
    free(bash);
    CloseHandle(write_handle);
    CloseHandle(null_handle);
    if (started != 0) {
        CloseHandle(read_handle);
        errno = ENOENT;
        return -1;
    }

    int fd = _open_osfhandle((intptr_t)read_handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(read_handle);
        TerminateJobObject(child.job, 1);
        (void)child_finish(&child, INFINITE, 0);
        return -1;
    }
    child_publish(&child);
    *pid = (pid_t)child.process_info.dwProcessId;
    *output_fd = fd;
    return 0;
}

void spawn_win32_terminate(pid_t pid)
{
    AcquireSRWLockShared(&child_lock);
    struct child_record *record = children;
    while (record && record->pid != pid)
        record = record->next;
    if (record)
        TerminateJobObject(record->job, 1);
    ReleaseSRWLockShared(&child_lock);
}

int spawn_win32_exit_seen(pid_t pid, int *exit_seen)
{
    AcquireSRWLockShared(&child_lock);
    struct child_record *record = children;
    while (record && record->pid != pid)
        record = record->next;
    if (!record) {
        ReleaseSRWLockShared(&child_lock);
        errno = ECHILD;
        return -1;
    }
    if (WaitForSingleObject(record->process, 0) == WAIT_OBJECT_0)
        *exit_seen = 1;
    ReleaseSRWLockShared(&child_lock);
    return 0;
}

pid_t spawn_win32_waitpid(pid_t pid, int *status, int options)
{
    AcquireSRWLockShared(&child_lock);
    struct child_record *found = children;
    while (found && found->pid != pid)
        found = found->next;
    if (!found) {
        ReleaseSRWLockShared(&child_lock);
        errno = ECHILD;
        return -1;
    }
    DWORD wait = WaitForSingleObject(found->process, options & WNOHANG ? 0 : INFINITE);
    ReleaseSRWLockShared(&child_lock);
    if (wait == WAIT_TIMEOUT)
        return 0;
    if (wait != WAIT_OBJECT_0)
        return -1;

    struct child_record *record = child_take(pid);
    if (!record) {
        errno = ECHILD;
        return -1;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(record->process, &exit_code);
    if (status)
        *status = (int)(exit_code & 0xff);
    CloseHandle(record->process);
    CloseHandle(record->job);
    free(record);
    return pid;
}

char *spawn_shell_cmd_force_utf8(char *shell_cmd)
{
    return shell_cmd;
}

void spawn_parent_ignore_signals(struct spawn_signal_state *state)
{
    memset(state, 0, sizeof(*state));
}

void spawn_parent_restore_signals(const struct spawn_signal_state *state)
{
    (void)state;
}

void spawn_child_reset_signals(void)
{
}

void spawn_child_redirect_stdio_to_null(void)
{
}

void spawn_child_die_with_parent(pid_t parent_pid, int signal_number)
{
    (void)parent_pid;
    (void)signal_number;
}

int spawn_shell_wait(const char *shell_cmd)
{
    wchar_t *bash = find_git_bash();
    wchar_t *command_line = bash ? bash_command_line(bash, shell_cmd) : NULL;
    if (!bash || !command_line) {
        free(bash);
        free(command_line);
        errno = ENOENT;
        return -1;
    }
    struct child_start child;
    int result = start_process(bash, command_line, GetStdHandle(STD_INPUT_HANDLE),
                               GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE),
                               NULL, &child);
    free(command_line);
    free(bash);
    return result == 0 ? child_finish(&child, INFINITE, 0) : -1;
}

enum spawn_pipe_mode {
    SPAWN_PIPE_READ,
    SPAWN_PIPE_WRITE,
};

static int pipe_open(struct spawn_pipe *result, const char *shell_cmd, enum spawn_pipe_mode mode)
{
    memset(result, 0, sizeof(*result));
    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE read_handle;
    HANDLE write_handle;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0))
        return -1;
    HANDLE parent_handle = mode == SPAWN_PIPE_READ ? read_handle : write_handle;
    HANDLE child_handle = mode == SPAWN_PIPE_READ ? write_handle : read_handle;
    SetHandleInformation(parent_handle, HANDLE_FLAG_INHERIT, 0);

    wchar_t *bash = find_git_bash();
    wchar_t *command_line = bash ? bash_command_line(bash, shell_cmd) : NULL;
    struct child_start child;
    int started =
        bash && command_line
            ? start_process(
                  bash, command_line,
                  mode == SPAWN_PIPE_WRITE ? child_handle : GetStdHandle(STD_INPUT_HANDLE),
                  mode == SPAWN_PIPE_READ ? child_handle : GetStdHandle(STD_OUTPUT_HANDLE),
                  GetStdHandle(STD_ERROR_HANDLE), NULL, &child)
            : -1;
    free(command_line);
    free(bash);
    CloseHandle(child_handle);
    if (started != 0) {
        CloseHandle(parent_handle);
        errno = ENOENT;
        return -1;
    }

    int flags = mode == SPAWN_PIPE_READ ? _O_RDONLY : _O_WRONLY;
    int fd = _open_osfhandle((intptr_t)parent_handle, flags | _O_BINARY);
    FILE *stream = fd >= 0 ? _fdopen(fd, mode == SPAWN_PIPE_READ ? "rb" : "wb") : NULL;
    if (!stream) {
        if (fd >= 0)
            _close(fd);
        else
            CloseHandle(parent_handle);
        TerminateJobObject(child.job, 1);
        (void)child_finish(&child, INFINITE, 0);
        return -1;
    }
    child_publish(&child);
    result->stream = stream;
    result->pid = (pid_t)child.process_info.dwProcessId;
    return 0;
}

int spawn_pipe_open_write(struct spawn_pipe *pipe, const char *shell_cmd)
{
    return pipe_open(pipe, shell_cmd, SPAWN_PIPE_WRITE);
}

int spawn_pipe_open_read(struct spawn_pipe *pipe, const char *shell_cmd)
{
    return pipe_open(pipe, shell_cmd, SPAWN_PIPE_READ);
}

int spawn_pipe_close(struct spawn_pipe *pipe)
{
    if (!pipe || !pipe->stream)
        return 0;
    fclose(pipe->stream);
    pipe->stream = NULL;
    struct child_record *record = child_take(pipe->pid);
    pipe->pid = 0;
    if (!record)
        return -1;
    struct child_start child = {.process_info = {.hProcess = record->process}, .job = record->job};
    free(record);
    return child_finish(&child, INFINITE, 0);
}

int spawn_wait_child(pid_t pid)
{
    struct child_record *record = child_take(pid);
    if (!record)
        return -1;
    struct child_start child = {.process_info = {.hProcess = record->process}, .job = record->job};
    free(record);
    return child_finish(&child, INFINITE, 0);
}

int spawn_wait_child_timeout(pid_t pid, int timeout_ms)
{
    struct child_record *record = child_take(pid);
    if (!record)
        return -1;
    struct child_start child = {.process_info = {.hProcess = record->process}, .job = record->job};
    free(record);
    return child_finish(&child, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms, 1);
}

int spawn_reap_if_exited(pid_t pid)
{
    AcquireSRWLockShared(&child_lock);
    struct child_record *record = children;
    while (record && record->pid != pid)
        record = record->next;
    int exited = record && WaitForSingleObject(record->process, 0) == WAIT_OBJECT_0;
    ReleaseSRWLockShared(&child_lock);
    if (exited)
        (void)spawn_wait_child(pid);
    return exited;
}

char *spawn_capture_stdout(const char *const *argv, size_t max_bytes, int timeout_ms,
                           size_t *out_len)
{
    if (!argv || !argv[0] || !out_len || timeout_ms <= 0) {
        errno = EINVAL;
        return NULL;
    }
    wchar_t *application = search_executable(argv[0]);
    wchar_t *command_line = application ? argv_command_line(argv) : NULL;
    if (!application || !command_line) {
        free(application);
        free(command_line);
        return NULL;
    }

    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE read_handle;
    HANDLE write_handle;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0)) {
        free(application);
        free(command_line);
        return NULL;
    }
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_handle =
        CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &security, OPEN_EXISTING, 0, NULL);
    struct child_start child;
    int started = start_process(application, command_line, null_handle, write_handle, null_handle,
                                NULL, &child);
    CloseHandle(write_handle);
    CloseHandle(null_handle);
    free(application);
    free(command_line);
    if (started != 0) {
        CloseHandle(read_handle);
        return NULL;
    }

    struct buf output;
    buf_init(&output);
    long deadline = monotonic_ms() + timeout_ms;
    int failed = 0;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_handle, NULL, 0, NULL, &available, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE)
                break;
            failed = 1;
            break;
        }
        if (available) {
            char chunk[65536];
            DWORD request = available < sizeof(chunk) ? available : sizeof(chunk);
            DWORD bytes_read = 0;
            if (!ReadFile(read_handle, chunk, request, &bytes_read, NULL)) {
                failed = 1;
                break;
            }
            if ((size_t)bytes_read > max_bytes - output.len) {
                failed = 1;
                break;
            }
            buf_append(&output, chunk, bytes_read);
            continue;
        }
        if (WaitForSingleObject(child.process_info.hProcess, 0) == WAIT_OBJECT_0)
            break;
        if (monotonic_ms() >= deadline) {
            failed = 1;
            break;
        }
        Sleep(2);
    }
    CloseHandle(read_handle);
    int status = child_finish(&child, failed ? 0 : (DWORD)(deadline - monotonic_ms()), 1);
    if (failed || status != 0 || output.len == 0) {
        buf_free(&output);
        return NULL;
    }
    *out_len = output.len;
    return buf_steal(&output);
}

#endif /* _WIN32 */
