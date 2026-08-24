/* SPDX-License-Identifier: MIT */
#ifdef _WIN32

#include <aclapi.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <curl/curl.h>
#include <sys/file.h>

#include "config.h"
#include "harness.h"
#include "session.h"
#include "tool.h"
#include "system/path.h"
#include "system/spawn.h"
#include "tools/task_registry.h"

static void test_curl_uses_schannel(void)
{
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    EXPECT(version != NULL);
    EXPECT(version && version->ssl_version && strstr(version->ssl_version, "Schannel") != NULL);
    EXPECT(version && (!version->ssl_version || strstr(version->ssl_version, "OpenSSL") == NULL));
}

static void test_git_bash_validation(void)
{
    EXPECT(spawn_win32_bash_available());
}

static void test_native_capture(void)
{
    const char *const argv[] = {"git.exe", "--version", NULL};
    size_t length = 0;
    char *output = spawn_capture_stdout(argv, 1024, 3000, &length);
    EXPECT(output != NULL);
    EXPECT(output && length > 4 && strncmp(output, "git version ", 12) == 0);
    free(output);
}

static void test_canonical_cwd(void)
{
    char *cwd = getcwd(NULL, 0);
    EXPECT(cwd != NULL);
    EXPECT(cwd && cwd[0] == '/' && cwd[2] == '/');
    free(cwd);
}

static void test_advisory_file_lock(void)
{
    char path[] = "hax_lock_XXXXXX";
    int first = mkstemp(path);
    int second = open(path, O_RDWR);
    EXPECT(first >= 0);
    EXPECT(second >= 0);
    if (first >= 0) {
        EXPECT(flock(first, LOCK_SH) == 0);
        EXPECT(write(first, "x", 1) == 1);
    }
    if (second >= 0) {
        EXPECT(flock(second, LOCK_EX | LOCK_NB) < 0);
        if (first >= 0)
            EXPECT(flock(first, LOCK_UN) == 0);
        EXPECT(flock(second, LOCK_EX | LOCK_NB) == 0);
        EXPECT(flock(second, LOCK_UN) == 0);
        close(second);
    }
    if (first >= 0)
        close(first);
    unlink(path);
}

static void test_stdio_mode_extensions(void)
{
    char path[] = "hax_stdio_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd >= 0)
        close(fd);

    FILE *file = fopen(path, "we");
    EXPECT(file != NULL);
    if (file) {
        EXPECT(fputs("logged", file) >= 0);
        EXPECT(fclose(file) == 0);
    }
    size_t length = 0;
    char *content = slurp_file(path, &length);
    EXPECT(content != NULL && length == 6);
    EXPECT(content && memcmp(content, "logged", 6) == 0);
    free(content);
    unlink(path);
}

static void test_positional_read_preserves_offset(void)
{
    char path[] = "hax_pread_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd < 0)
        return;
    EXPECT(write(fd, "0123456789", 10) == 10);
    EXPECT(lseek(fd, 3, SEEK_SET) == 3);
    char bytes[3] = {0};
    EXPECT(pread(fd, bytes, 2, 7) == 2);
    EXPECT(memcmp(bytes, "78", 2) == 0);
    EXPECT(lseek(fd, 0, SEEK_CUR) == 3);
    close(fd);
    unlink(path);
}

static void test_open_guarantees(void)
{
    char path[] = "hax_open_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd >= 0) {
        DWORD flags = HANDLE_FLAG_INHERIT;
        HANDLE handle = (HANDLE)_get_osfhandle(fd);
        EXPECT(GetHandleInformation(handle, &flags));
        EXPECT(!(flags & HANDLE_FLAG_INHERIT));
        errno = 0;
        EXPECT(fchmod(fd, 0644) < 0 && errno == ENOTSUP);
        close(fd);
    }

    errno = 0;
    fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    EXPECT(fd < 0 && errno == ENOTDIR);
    fd = open(t_tempdir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    EXPECT(fd >= 0);
    if (fd >= 0)
        close(fd);

    errno = 0;
    fd = open("NUL", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    EXPECT(fd < 0);

    char *link_path = xasprintf("%s-link", path);
    if (symlink(path, link_path) == 0) {
        errno = 0;
        fd = open(link_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        EXPECT(fd < 0 && errno == ELOOP);
        unlink(link_path);
    }
    free(link_path);
    unlink(path);
}

static void test_private_file_acl(void)
{
    char path[] = "hax_acl_XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    if (fd < 0)
        return;
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    PSID owner = NULL;
    PACL acl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    DWORD result = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                   NULL, &acl, NULL, &descriptor);
    EXPECT(result == ERROR_SUCCESS);
    ACL_SIZE_INFORMATION info = {0};
    EXPECT(result != ERROR_SUCCESS ||
           GetAclInformation(acl, &info, sizeof(info), AclSizeInformation));
    EXPECT(result != ERROR_SUCCESS || info.AceCount == 1);
    if (result == ERROR_SUCCESS && info.AceCount == 1) {
        ACCESS_ALLOWED_ACE *ace = NULL;
        EXPECT(GetAce(acl, 0, (void **)&ace));
        EXPECT(ace && EqualSid(owner, &ace->SidStart));
    }
    LocalFree(descriptor);
    close(fd);
    unlink(path);
}

static void test_background_output_stream(void)
{
    config_set_override("bash.background_yield", "1ms");
    char *launch =
        TOOL_BASH.run("{\"command\":\"for i in $(seq 1 1000); do printf '%04d\\\\n' \\\"$i\\\"; "
                      "done; sleep 0.2\",\"background\":true,\"name\":\"win-race\"}",
                      NULL);
    EXPECT(launch != NULL && strstr(launch, "task win-race") != NULL);
    char *result = task_wait_stream("win-race", 30000, 0, NULL, NULL);
    EXPECT(result != NULL);
    char *combined = xasprintf("%s%s", launch ? launch : "", result ? result : "");
    EXPECT(strstr(combined, "0001\n") != NULL);
    EXPECT(strstr(combined, "1000\n") != NULL);
    EXPECT(strstr(combined, "finished (exit 0)") != NULL);
    free(combined);
    free(launch);
    free(result);
    task_registry_shutdown();
}

static void test_session_materialization(void)
{
    setenv("XDG_STATE_HOME", t_tempdir(), 1);
    unsetenv("HAX_NO_SESSION");
    struct session_log *log = session_log_open("mock", "mock-model", NULL, NULL, NULL);
    EXPECT(log != NULL);
    if (!log)
        return;

    struct item items[] = {{.kind = ITEM_TURN_BOUNDARY},
                           {.kind = ITEM_USER_MESSAGE, .text = (char *)"ping"}};
    session_log_append(log, items, sizeof(items) / sizeof(*items));
    EXPECT(session_log_materialized(log));
    const char *path = session_log_path(log);
    EXPECT(path != NULL && access(path, F_OK) == 0);
    session_log_close(log);
}

int main(void)
{
    test_curl_uses_schannel();
    test_git_bash_validation();
    test_native_capture();
    test_canonical_cwd();
    test_advisory_file_lock();
    test_stdio_mode_extensions();
    test_positional_read_preserves_offset();
    test_open_guarantees();
    test_private_file_acl();
    test_background_output_stream();
    test_session_materialization();
    T_REPORT();
}

#endif /* _WIN32 */
