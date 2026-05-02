/* SPDX-License-Identifier: MIT */
#ifndef HAX_CMD_CLASSIFY_H
#define HAX_CMD_CLASSIFY_H

/* Returns nonzero if the bash command looks like pure exploration
 * (read-only file content / search / listing). The agent uses this to
 * suppress the dim output preview for a tool call — a misclassification
 * only affects display, never correctness, since the model still sees
 * the full canonical output. Approximation, not a security boundary;
 * see the top-of-file comment in cmd_classify.c for the rationale and
 * the categories of false negatives accepted by design. */
int cmd_is_exploration(const char *cmd);

#endif /* HAX_CMD_CLASSIFY_H */
