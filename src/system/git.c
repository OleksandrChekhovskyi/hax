/* SPDX-License-Identifier: MIT */
#include "system/git.h"

#include <stdlib.h>
#include <string.h>

#include "xalloc.h"
#include "system/spawn.h"

/* The probe runs before the first request goes out, so a git that can't answer about the working
 * directory promptly — a contended lock, a stalled network mount — loses its label rather than
 * holding up the session. */
#define GIT_TIMEOUT_MS 1000
#define GIT_MAX_BYTES  8192

/* Returns the command's output without its trailing newline, or NULL when it failed or said
 * nothing. */
static char *run_git(const char *const *argv)
{
    size_t length = 0;
    char *output = spawn_capture_stdout(argv, GIT_MAX_BYTES, GIT_TIMEOUT_MS, &length);
    if (!output)
        return NULL;
    while (length > 0 && (output[length - 1] == '\n' || output[length - 1] == '\r'))
        output[--length] = '\0';
    if (length == 0) {
        free(output);
        return NULL;
    }
    return output;
}

void git_state_probe(struct git_state *out)
{
    *out = (struct git_state){0};

    /* symbolic-ref rather than rev-parse --abbrev-ref: a detached HEAD is an error exit instead of
     * the literal branch name "HEAD". */
    static const char *const branch_argv[] = {"git",     "symbolic-ref", "--quiet",
                                              "--short", "HEAD",         NULL};
    static const char *const head_argv[] = {"git", "log", "-1", "--format=%h%n%s", NULL};

    out->branch = run_git(branch_argv);

    char *head = run_git(head_argv);
    if (!head)
        return;
    char *newline = strchr(head, '\n');
    if (newline) {
        *newline = '\0';
        out->subject = xstrdup(newline + 1);
    }
    out->commit = head;
}

void git_state_free(struct git_state *state)
{
    free(state->branch);
    free(state->commit);
    free(state->subject);
    *state = (struct git_state){0};
}
