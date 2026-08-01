/* SPDX-License-Identifier: MIT */
#ifndef HAX_GIT_H
#define HAX_GIT_H

/* Repository position at a point in time. Recorded with a session so a conversation whose opening
 * prompt is generic ("review the last commit") stays identifiable later. */
struct git_state {
    char *branch;  /* NULL when HEAD is detached or the directory is not a repository */
    char *commit;  /* abbreviated HEAD hash; NULL on an unborn branch */
    char *subject; /* HEAD commit subject, always a single line */
};

/* Fills out by running git in the current directory. Every field is independently optional: a
 * missing git, a non-repository, or a failing command leaves it NULL. */
void git_state_probe(struct git_state *out);
void git_state_free(struct git_state *state);

#endif /* HAX_GIT_H */
