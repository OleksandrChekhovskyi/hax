/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "system/os.h"
#include "util.h"

static char *write_release(const char *content)
{
    char *path = xstrdup("/tmp/hax-os-release-test-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        FAIL("mkstemp: %s", strerror(errno));
        free(path);
        return NULL;
    }
    size_t len = strlen(content);
    ssize_t n = write(fd, content, len);
    if (n < 0 || (size_t)n != len)
        FAIL("write: %s", n < 0 ? strerror(errno) : "short write");
    close(fd);
    return path;
}

static void test_pretty_name(void)
{
    char *path = write_release("NAME=ignored\nPRETTY_NAME=\"Test Linux 1.0\"\n");
    if (!path)
        return;
    char *name = os_release_name(path);
    EXPECT_STR_EQ(name, "Test Linux 1.0");
    free(name);
    unlink(path);
    free(path);
}

static void test_name_and_version_fallback(void)
{
    char *path = write_release("NAME='Test Linux'\nVERSION=\"2 (Tree)\"\n");
    if (!path)
        return;
    char *name = os_release_name(path);
    EXPECT_STR_EQ(name, "Test Linux 2 (Tree)");
    free(name);
    unlink(path);
    free(path);
}

static void test_quoted_escapes(void)
{
    char *path = write_release("PRETTY_NAME=\"Test \\\"Linux\\\"\"\n");
    if (!path)
        return;
    char *name = os_release_name(path);
    EXPECT_STR_EQ(name, "Test \"Linux\"");
    free(name);
    unlink(path);
    free(path);
}

static void test_missing_identity(void)
{
    char *path = write_release("ID=test\n");
    if (!path)
        return;
    char *name = os_release_name(path);
    EXPECT(name == NULL);
    free(name);
    unlink(path);
    free(path);
}

static void test_duplicate_keys_use_last_value(void)
{
    char *path = write_release("PRETTY_NAME=old\nPRETTY_NAME=new\n");
    if (!path)
        return;
    char *name = os_release_name(path);
    EXPECT_STR_EQ(name, "new");
    free(name);
    unlink(path);
    free(path);
}

static void test_existing_primary_is_exclusive(void)
{
    char *primary = write_release("ID=override\n");
    if (!primary)
        return;
    char *fallback = write_release("PRETTY_NAME=fallback\n");
    if (!fallback) {
        unlink(primary);
        free(primary);
        return;
    }
    char *name = os_release_name_with_fallback(primary, fallback);
    EXPECT(name == NULL);
    free(name);
    unlink(primary);
    unlink(fallback);
    free(primary);
    free(fallback);
}

static void test_missing_primary_uses_fallback(void)
{
    char *fallback = write_release("PRETTY_NAME=fallback\n");
    if (!fallback)
        return;
    char *primary = xasprintf("%s-missing", fallback);
    char *name = os_release_name_with_fallback(primary, fallback);
    EXPECT_STR_EQ(name, "fallback");
    free(name);
    free(primary);
    unlink(fallback);
    free(fallback);
}

static void test_host_description(void)
{
    char *description = os_description();
    EXPECT(description != NULL && *description != '\0');
    free(description);
}

int main(void)
{
    test_pretty_name();
    test_name_and_version_fallback();
    test_quoted_escapes();
    test_missing_identity();
    test_duplicate_keys_use_last_value();
    test_existing_primary_is_exclusive();
    test_missing_primary_uses_fallback();
    test_host_description();
    T_REPORT();
}
