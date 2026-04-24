/* SPDX-License-Identifier: MIT */
#include "tool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "util.h"

static char *write_tmp(const void *data, size_t len)
{
    char *path = xstrdup("/tmp/hax-test-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        FAIL("mkstemp: %s", strerror(errno));
        free(path);
        return NULL;
    }
    if (len && write(fd, data, len) != (ssize_t)len)
        FAIL("short write to %s", path);
    close(fd);
    return path;
}

static char *call_read(const char *args_json)
{
    return TOOL_READ.run(args_json);
}

static void test_read_invalid_json(void)
{
    char *out = call_read("not json");
    EXPECT(strstr(out, "invalid arguments") != NULL);
    free(out);
}

static void test_read_missing_path(void)
{
    char *out = call_read("{}");
    EXPECT(strstr(out, "missing 'path'") != NULL);
    free(out);
}

static void test_read_empty_path(void)
{
    char *out = call_read("{\"path\":\"\"}");
    EXPECT(strstr(out, "missing 'path'") != NULL);
    free(out);
}

static void test_read_nonexistent(void)
{
    char *out = call_read("{\"path\":\"/nonexistent/path/should-not-exist\"}");
    EXPECT(strstr(out, "error reading") != NULL);
    free(out);
}

static void test_read_normal(void)
{
    const char content[] = "hello\nworld\n";
    char *path = write_tmp(content, sizeof(content) - 1);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, content);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_sanitizes_utf8(void)
{
    /* Embed a raw NUL and an invalid leading byte — both must become U+FFFD. */
    const char content[] = {'a', 0x00, 'b', (char)0xFF, 'c'};
    char *path = write_tmp(content, sizeof(content));
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT_STR_EQ(out, "a\xEF\xBF\xBD"
                       "b\xEF\xBF\xBD"
                       "c");
    free(out);
    free(args);
    unlink(path);
    free(path);
}

static void test_read_truncation_marker(void)
{
    /* READ_CAP is 256 KiB; write one byte past it and expect the marker. */
    size_t over = 256 * 1024 + 32;
    char *big = xmalloc(over);
    memset(big, 'q', over);
    char *path = write_tmp(big, over);
    free(big);
    char *args = xasprintf("{\"path\":\"%s\"}", path);
    char *out = call_read(args);
    EXPECT(strstr(out, "[truncated at") != NULL);
    EXPECT(strstr(out, "file is larger]") != NULL);
    free(out);
    free(args);
    unlink(path);
    free(path);
}

int main(void)
{
    test_read_invalid_json();
    test_read_missing_path();
    test_read_empty_path();
    test_read_nonexistent();
    test_read_normal();
    test_read_sanitizes_utf8();
    test_read_truncation_marker();
    T_REPORT();
}
