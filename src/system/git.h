/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_GIT_H
#define HAX_SYSTEM_GIT_H

/* Repository position at a point in time. Recorded with a session so a conversation whose opening
 * prompt is generic ("review the last commit") stays identifiable later. */
struct git_state {
    char *branch;  /* NULL when HEAD is detached or the directory is not a repository */
    char *commit;  /* abbreviated HEAD hash; NULL on an unborn branch */
    char *subject; /* HEAD commit subject, always a single line */
};

/* Fills out by running git in the current directory. Every field is independently optional: a
 * missing git, a directory outside a work tree, or a failing command leaves it NULL. */
void git_state_probe(struct git_state *out);
void git_state_free(struct git_state *state);

/* Return the nearest directory at or above absolute `dir` holding a `.git` entry: a repository's
 * directory, a linked worktree's file, or a symlink, which is not followed. Returns NULL when there
 * is none; the result is allocated. */
char *git_find_worktree_root(const char *dir);

#endif /* HAX_SYSTEM_GIT_H */
