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

/* Modal control-key bindings. The dispatch half needs a tty (it drops raw
 * mode and hands over the terminal), so these pin the registration rules
 * the editor guarantees to callers. */

static void noop_view(void *user)
{
    (void)user;
}

static void other_view(void *user)
{
    (void)user;
}

/* The naming macro must land on the C0 byte the terminal actually sends,
 * or a binding would be registered for a key nobody can press. */
static void test_modal_key_macro(void)
{
    EXPECT(INPUT_KEY_CTRL('O') == 0x0f);
    EXPECT(INPUT_KEY_CTRL('T') == 0x14);
    EXPECT(INPUT_KEY_CTRL('A') == 0x01);
}

static void test_modal_key_bind_and_rebind(void)
{
    struct input *in = input_new();
    int slot_a = 0;

    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), noop_view, &slot_a) == 0);
    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('T'), noop_view, NULL) == 0);
    EXPECT(in->modal_keys[0].key == INPUT_KEY_CTRL('O'));
    EXPECT(in->modal_keys[0].user == &slot_a);
    EXPECT(in->modal_keys[1].key == INPUT_KEY_CTRL('T'));

    /* Rebinding replaces in place rather than consuming a second slot —
     * otherwise a caller that re-registers on every prompt would exhaust
     * the table. */
    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), other_view, NULL) == 0);
    EXPECT(in->modal_keys[0].fn == other_view);
    EXPECT(in->modal_keys[0].user == NULL);
    EXPECT(in->modal_keys[2].fn == NULL);

    /* Clearing frees the slot for reuse. An empty slot always has
     * fn == NULL, which is also what keeps a stray NUL byte from matching
     * one during dispatch. */
    EXPECT(input_bind_modal_key(in, INPUT_KEY_CTRL('O'), NULL, NULL) == 0);
    EXPECT(in->modal_keys[0].fn == NULL);

    input_free(in);
}

/* Only control bytes can be bound: a printable key would shadow typing. */
static void test_modal_key_rejects_printable(void)
{
    struct input *in = input_new();
    EXPECT(input_bind_modal_key(in, 'q', noop_view, NULL) == -1);
    EXPECT(in->modal_keys[0].fn == NULL);
    input_free(in);
}

/* A full table refuses rather than dropping a binding on the floor, so an
 * over-eager caller finds out instead of losing a key silently. Clearing an
 * unbound key is not an error and consumes nothing. */
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
    test_modal_key_macro();
    test_modal_key_bind_and_rebind();
    test_modal_key_rejects_printable();
    test_modal_key_table_full();
    T_REPORT();
}
