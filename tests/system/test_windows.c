/* SPDX-License-Identifier: MIT */
#ifdef _WIN32

#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/file.h>

#include "harness.h"
#include "session.h"
#include "system/path.h"
#include "system/spawn.h"

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
    test_session_materialization();
    T_REPORT();
}

#endif /* _WIN32 */
