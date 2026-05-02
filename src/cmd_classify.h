/* SPDX-License-Identifier: MIT */
#ifndef HAX_CMD_CLASSIFY_H
#define HAX_CMD_CLASSIFY_H

/* Returns nonzero if the bash command looks like pure exploration
 * (read-only file content / search / listing). The agent uses this to
 * decide whether to suppress the dim output preview for a tool call —
 * a misclassification only affects display, never correctness, since
 * the model still sees the full canonical output.
 *
 * Heuristic, deliberately conservative:
 *   1. Reject if the command contains command substitution ($(, `),
 *      process substitution (<( >( ), heredocs (<<), or output
 *      redirection (>, >>). Stderr-merge forms like 2>&1 and
 *      2>/dev/null are tolerated.
 *   2. Tokenize on top-level &&, ||, ;, |, respecting single- and
 *      double-quoted strings and \-escapes.
 *   3. For each segment: strip leading `cd PATH`, `pushd PATH`,
 *      `popd`, and `env VAR=...` prefixes. The remaining leading word
 *      determines the segment's class.
 *   4. Drop "format-only" segments (head/tail without a file operand,
 *      wc, sort, uniq, cut, tr, awk, sed without file operand, column,
 *      tee without operand, non-mutating xargs).
 *   5. Every remaining segment must classify as READ / LIST / SEARCH.
 *      One UNKNOWN segment → exploration is 0.
 */
int cmd_is_exploration(const char *cmd);

#endif /* HAX_CMD_CLASSIFY_H */
