/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "harness.h"
#include "util.h"
#include "terminal/input.h"
#include "terminal/input_core.h"

/* History file I/O: the split between loading a file for recall and taking
 * ownership of it for appends. The tty-gated wrapper
 * (input_history_open_default) isn't reachable from a test binary, so these
 * drive the path-taking variants it dispatches to. */

static char *fixture(const char *body)
{
    char *path = xasprintf("%s/history", t_tempdir());
    FILE *f = fopen(path, "w");
    EXPECT(f != NULL);
    if (f) {
        fputs(body, f);
        fclose(f);
    }
    return path;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void test_history_load_is_read_only(void)
{
    char *path = fixture("first\nsecond\n");
    long before = file_size(path);

    struct input *in = input_new();
    input_history_load(in, path);

    /* Recall works: both records are in memory, oldest first. */
    EXPECT(in->hist_n == 2);
    EXPECT_STR_EQ(in->hist[0], "first");
    EXPECT_STR_EQ(in->hist[1], "second");

    /* ...but the file is not this run's to grow. input_history_add still
     * records in memory (so Up-arrow recalls what was typed this run), it
     * just has no persist path to append through. */
    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 3);
    EXPECT_STR_EQ(in->hist[2], "typed this run");
    EXPECT(file_size(path) == before);

    input_free(in);
    free(path);
}

static void test_history_open_appends(void)
{
    char *path = fixture("first\nsecond\n");
    long before = file_size(path);

    struct input *in = input_new();
    input_history_open(in, path);
    EXPECT(in->hist_n == 2);

    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 3);
    EXPECT(file_size(path) > before);

    /* Reload into a fresh editor: the appended line survived the run. */
    struct input *in2 = input_new();
    input_history_load(in2, path);
    EXPECT(in2->hist_n == 3);
    EXPECT_STR_EQ(in2->hist[2], "typed this run");

    input_free(in2);
    input_free(in);
    free(path);
}

/* A missing file is not an error for either variant, and load must not
 * create one on the way past (open's mkdir -p has no counterpart here). */
static void test_history_missing_file(void)
{
    char *path = xasprintf("%s/nope/history", t_tempdir());

    struct input *in = input_new();
    input_history_load(in, path);
    EXPECT(in->hist_n == 0);
    EXPECT(file_size(path) == -1);

    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 1);
    EXPECT(file_size(path) == -1);

    input_free(in);
    free(path);
}

int main(void)
{
    test_history_load_is_read_only();
    test_history_open_appends();
    test_history_missing_file();
    T_REPORT();
}
