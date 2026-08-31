/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "harness.h"
#include "xalloc.h"
#include "system/path.h"
#include "tools/path_preprocess.h"

static const char *test_cwd;

static json_t *load_rewritten_args(const char *args_json)
{
    char *rewritten = tool_relativize_path_args(args_json);
    EXPECT(rewritten != NULL);
    if (!rewritten)
        return NULL;

    json_t *args = json_loads(rewritten, 0, NULL);
    EXPECT(args != NULL);
    free(rewritten);
    return args;
}

static void expect_no_rewrite(const char *args_json)
{
    char *rewritten = tool_relativize_path_args(args_json);
    EXPECT(rewritten == NULL);
    free(rewritten);
}

static void test_rewrites_descendant_path(void)
{
    char *absolute_path = path_join(test_cwd, "src/file.c");
    char *args_json = xasprintf("{\"path\":\"%s\",\"offset\":3}", absolute_path);
    json_t *args = load_rewritten_args(args_json);

    if (args) {
        const char *path = json_string_value(json_object_get(args, "path"));
        EXPECT(path != NULL);
        if (path)
            EXPECT_STR_EQ(path, "src/file.c");
        EXPECT(json_integer_value(json_object_get(args, "offset")) == 3);
        json_decref(args);
    }

    free(args_json);
    free(absolute_path);
}

static void test_expands_home_before_rewriting(void)
{
    setenv("HOME", test_cwd, 1);
    json_t *args = load_rewritten_args("{\"path\":\"~/src/file.c\"}");
    if (!args)
        return;

    const char *path = json_string_value(json_object_get(args, "path"));
    EXPECT(path != NULL);
    if (path)
        EXPECT_STR_EQ(path, "src/file.c");
    json_decref(args);
}

static void test_returns_null_when_no_rewrite_applies(void)
{
    expect_no_rewrite(NULL);
    expect_no_rewrite("{");
    expect_no_rewrite("[]");
    expect_no_rewrite("{\"path\":3}");
    expect_no_rewrite("{\"path\":\"src/file.c\"}");
    expect_no_rewrite("{\"path\":\"/etc/hosts\"}");

    char *args_json = xasprintf("{\"path\":\"%s\"}", test_cwd);
    expect_no_rewrite(args_json);
    free(args_json);
}

int main(void)
{
    test_cwd = t_tempdir();
    if (chdir(test_cwd) < 0) {
        perror("chdir");
        return 1;
    }

    test_rewrites_descendant_path();
    test_expands_home_before_rewriting();
    test_returns_null_when_no_rewrite_applies();

    T_REPORT();
}
