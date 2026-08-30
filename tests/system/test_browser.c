/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#include "harness.h"
#include "util.h"
#include "system/browser.h"
#include "system/fs.h"

/* Both platform opener names point at the same recorder so the test is platform-independent.
 * The recording is published by rename, so a poll never observes a partially written file. */
static void write_fake_opener(const char *dir, const char *name, const char *out_path)
{
    char *path = xasprintf("%s/%s", dir, name);
    FILE *script = fopen(path, "w");
    EXPECT(script != NULL);
    if (script) {
        /* Absolute command paths: the test replaces PATH with just the fixture directory, and
         * printf is not a builtin in every /bin/sh (OpenBSD's pdksh). */
        fprintf(script,
                "#!/bin/sh\n/usr/bin/printf '%%s' \"$1\" > '%s.tmp' && /bin/mv '%s.tmp' '%s'\n",
                out_path, out_path, out_path);
        fclose(script);
        EXPECT(chmod(path, 0755) == 0);
    }
    free(path);
}

static void test_hands_url_to_opener(void)
{
    char *dir = t_tempdir();
    char *out_path = xasprintf("%s/url.txt", dir);
    write_fake_opener(dir, "open", out_path);
    write_fake_opener(dir, "xdg-open", out_path);

    char *saved_path = xstrdup(getenv("PATH"));
    setenv("PATH", dir, 1);
    browser_open_url("https://example.test/authorize?x=1&y=2");
    setenv("PATH", saved_path ? saved_path : "", 1);
    free(saved_path);

    /* The opener runs detached, so the recording lands asynchronously. */
    char *recorded = NULL;
    for (int i = 0; i < 300 && !recorded; i++) {
        recorded = slurp_file(out_path, NULL);
        if (!recorded) {
            struct timespec pause = {.tv_nsec = 10 * 1000 * 1000};
            nanosleep(&pause, NULL);
        }
    }
    EXPECT(recorded != NULL);
    if (recorded)
        EXPECT_STR_EQ(recorded, "https://example.test/authorize?x=1&y=2");
    free(recorded);
    free(out_path);
}

int main(void)
{
    test_hands_url_to_opener();
    T_REPORT();
}
