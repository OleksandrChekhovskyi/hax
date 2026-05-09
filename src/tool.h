/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_H
#define HAX_TOOL_H

#include "provider.h"

/*
 * A tool bundles its metadata (passed to the model) with the function that
 * runs it. Each tool lives in its own translation unit under src/tools/ and
 * exports exactly one `const struct tool` symbol.
 *
 * run() takes a JSON-encoded argument string and returns the canonical
 * tool output that goes into conversation history (and back to the
 * model). Returning NULL is not allowed; an error message is itself
 * valid output the model can recover from. Returning "" is fine.
 *
 * The `emit_display` callback is a display-only side channel: bytes fed
 * through it flow into the agent's head/tail preview renderer but never
 * enter conversation history. This decouples "what the user sees on
 * screen" from "what the model remembers", and tools use it in two ways:
 *
 *   - Streaming progress: `bash` calls emit_display per pipe-read chunk
 *     so the user sees output live, and returns a head+tail-truncated,
 *     UTF-8 sanitized summary as canonical output so the model isn't
 *     billed for an unbounded live stream.
 *
 *   - Avoiding redundant context: `write` on a new file pushes the
 *     content through emit_display for a `read`-style preview, and
 *     returns a short "created <path>" confirmation as canonical output
 *     — the content is already in the call arguments, no need to echo
 *     it back.
 *
 * A tool that has nothing display-only to say just ignores the
 * emit_display params; the agent feeds the returned string through the
 * callback once on completion so the preview pipeline runs uniformly
 * either way.
 *
 * output_is_diff hints that successful output is a unified diff and the
 * agent should render it colored (and uncapped) instead of as a dim
 * preview. Failure messages from the same tool are not diffs; the agent
 * routes by checking whether the output starts with `--- `.
 */
typedef int (*tool_emit_display_fn)(const char *bytes, size_t n, void *user);

struct tool {
    struct tool_def def;
    char *(*run)(const char *args_json, tool_emit_display_fn emit_display, void *user);
    /* Optional. Returns a small dim suffix appended to the tool-call header
     * after the bold `display_arg` value — e.g. `:5-20` for `read` to
     * surface the requested line range. NULL or empty string means no
     * suffix. Caller frees the returned string. */
    char *(*format_display_extra)(const char *args_json);
    int output_is_diff;
    /* Maximum rows the verbose tool-call header may occupy. The
     * display_arg word-wraps at row boundaries up to this cap, then
     * truncates with "..."; 0 (the default) is treated as 1. Bash
     * sets this to 3 because shell commands carry real signal that's
     * worth seeing across multiple rows; tools with a path-shaped
     * arg leave it at the default. */
    int header_rows;
    /* When set, the dim preview shows both ends of long output with the
     * middle elided — useful for command output where errors/summaries
     * tend to land at the bottom. The default (head-only) is right for
     * tools whose output is read top-down (file content). */
    int preview_tail;
    /* When set, the call renders as a one-line header with no preview
     * body and an inline spinner — used for read-only exploration tools
     * whose output is for the model, not the user. Consecutive silent
     * calls of the same tool can coalesce into one line (see agent.c
     * read coalescing). */
    int silent_preview;
    /* Optional per-call override of silent_preview. If set, the agent
     * calls this with the tool's args_json and treats the call as
     * silent iff the function returns nonzero. Used by `bash` to
     * classify exploration commands (ls/grep/find/...) at runtime. */
    int (*is_silent)(const char *args_json);
};

extern const struct tool TOOL_READ;
extern const struct tool TOOL_BASH;
extern const struct tool TOOL_WRITE;
extern const struct tool TOOL_EDIT;

/* Unlink and forget every bash temp file we still hold — `bash` keeps
 * the file when output truncates so the model can `read` it on the next
 * turn, but those become unreachable once the conversation is reset.
 * Called from agent_new_conversation (/new) and from an atexit handler
 * registered on first spill. atexit doesn't fire on signal-driven exits
 * (Ctrl-C, SIGHUP, kill), so a signalled session can still leak; the OS
 * eventually evicts /tmp / /var/folders. */
void bash_cleanup_tempfiles(void);

#endif /* HAX_TOOL_H */
