/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "harness.h"
#include "xalloc.h"
#include "terminal/input.h"
#include "terminal/input_core.h"

static char *history_fixture(const char *body)
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
    char *path = history_fixture("first\nsecond\n");
    long before = file_size(path);

    struct input *in = input_new();
    input_history_load(in, path);

    EXPECT(in->hist_n == 2);
    EXPECT_STR_EQ(in->hist[0], "first");
    EXPECT_STR_EQ(in->hist[1], "second");

    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 3);
    EXPECT_STR_EQ(in->hist[2], "typed this run");
    EXPECT(file_size(path) == before);

    input_free(in);
    free(path);
}

static void test_history_open_appends(void)
{
    char *path = history_fixture("first\nsecond\n");
    long before = file_size(path);

    struct input *in = input_new();
    input_history_open(in, path);
    EXPECT(in->hist_n == 2);

    input_history_add(in, "typed this run");
    EXPECT(in->hist_n == 3);
    EXPECT(file_size(path) > before);

    struct input *reloaded = input_new();
    input_history_load(reloaded, path);
    EXPECT(reloaded->hist_n == 3);
    EXPECT_STR_EQ(reloaded->hist[2], "typed this run");

    input_free(reloaded);
    input_free(in);
    free(path);
}

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

static void test_history_session_entry_persists_on_resubmit(void)
{
    char *path = history_fixture("first\n");

    struct input *in = input_new();
    input_history_open(in, path);
    long before = file_size(path);

    input_history_add_session(in, "stashed draft");
    EXPECT(in->hist_n == 2);
    EXPECT(file_size(path) == before);

    /* Submitting the recalled draft unchanged must reach the file despite being
     * a repeat of the newest in-memory entry. */
    input_history_add(in, "stashed draft");
    EXPECT(in->hist_n == 2);
    long after = file_size(path);
    EXPECT(after > before);

    input_history_add(in, "stashed draft");
    EXPECT(file_size(path) == after);

    struct input *reloaded = input_new();
    input_history_load(reloaded, path);
    EXPECT(reloaded->hist_n == 2);
    EXPECT_STR_EQ(reloaded->hist[1], "stashed draft");

    input_free(reloaded);
    input_free(in);
    free(path);
}

static void noop_view(void *user)
{
    (void)user;
}

static void other_view(void *user)
{
    (void)user;
}

static void test_modal_key_macro(void)
{
    EXPECT(INPUT_KEY_CTRL('O') == 0x0f);
    EXPECT(INPUT_KEY_CTRL('T') == 0x14);
    EXPECT(INPUT_KEY_CTRL('A') == 0x01);
}

static void test_modal_key_bind_and_rebind(void)
{
    struct input *in = input_new();
    int binding_user = 0;

    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), noop_view, &binding_user) == 0);
    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('T'), noop_view, NULL) == 0);
    EXPECT(in->modal_keys[0].key == INPUT_KEY_CTRL('O'));
    EXPECT(in->modal_keys[0].user == &binding_user);
    EXPECT(in->modal_keys[1].key == INPUT_KEY_CTRL('T'));

    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), other_view, NULL) == 0);
    EXPECT(in->modal_keys[0].fn == other_view);
    EXPECT(in->modal_keys[0].user == NULL);
    EXPECT(in->modal_keys[2].fn == NULL);

    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), NULL, NULL) == 0);
    EXPECT(in->modal_keys[0].fn == NULL);

    input_free(in);
}

static void test_modal_key_rejects_printable(void)
{
    struct input *in = input_new();
    EXPECT(input_bind_modal_key(in, 'q', noop_view, NULL) == -1);
    EXPECT(in->modal_keys[0].fn == NULL);
    input_free(in);
}

static void test_modal_key_table_full(void)
{
    struct input *in = input_new();
    for (int i = 0; i < INPUT_MODAL_KEYS_MAX; i++)
        EXPECT(input_bind_modal_key(in, (unsigned char)(i + 1), noop_view, NULL) == 0);
    EXPECT(input_bind_modal_key(in, 0x1f, noop_view, NULL) == -1);
    EXPECT(input_bind_modal_key(in, 0x1f, NULL, NULL) == 0);
    input_free(in);
}

int main(void)
{
    test_history_load_is_read_only();
    test_history_open_appends();
    test_history_missing_file();
    test_history_session_entry_persists_on_resubmit();
    test_modal_key_macro();
    test_modal_key_bind_and_rebind();
    test_modal_key_rejects_printable();
    test_modal_key_table_full();
    T_REPORT();
}
