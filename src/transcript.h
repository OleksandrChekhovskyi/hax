/* SPDX-License-Identifier: MIT */
#ifndef HAX_TRANSCRIPT_H
#define HAX_TRANSCRIPT_H

#include <stddef.h>
#include <stdio.h>

struct item;
struct tool_def;

/* Render the conversation as the model sees it — system prompt, advertised
 * tools, then each item in order, all unshortened. ANSI styling is
 * included so the output pages cleanly through `less -R`. Pure
 * formatting: no popen, no isatty checks, no errno handling — the caller
 * decides where bytes go. Pass tools=NULL / n_tools=0 to omit the tools
 * section (matches --raw mode, where no tools are advertised). */
void transcript_render(FILE *out, const char *system_prompt, const struct tool_def *tools,
                       size_t n_tools, const struct item *items, size_t n_items);

/* Render just the banner + system prompt + tools sections. Shared between
 * the one-shot Ctrl-T view (transcript_render) and the streaming
 * HAX_TRANSCRIPT log (transcript_log_open). `color` selects ANSI
 * styling (1, for `less -R`) vs plain text (0, for cat/grep/agents). */
void transcript_render_header(FILE *out, int color, const char *system_prompt,
                              const struct tool_def *tools, size_t n_tools);

/* Render items[start_idx .. n_items). `color` matches transcript_render_header.
 * *turn_no is the running turn counter (incremented on each
 * ITEM_TURN_BOUNDARY) — pass &0 for a one-shot render, or carry the
 * value across calls when streaming an append-only log. Tool-call/result
 * pairing requires that any CALL in the slice has its matching RESULT in
 * the same slice; callers should therefore only invoke this at points
 * where pending tool batches have completed (end of a model round-trip
 * is the natural choice). */
void transcript_render_items(FILE *out, int color, const struct item *items, size_t n_items,
                             size_t start_idx, int *turn_no);

/* Truncate the HAX_TRANSCRIPT file (if set) without writing anything
 * else. Called from main before any provider/session setup so a
 * config-error exit path leaves an empty file rather than stale
 * content from a previous run. transcript_log_open later writes the
 * header once sys+tools are known. No-op when HAX_TRANSCRIPT is unset.
 * Errors are silent: a later transcript_log_open failure surfaces the
 * same diagnostic at the moment the session actually needs the file. */
void transcript_log_init(void);

/* Transcript log driven by the HAX_TRANSCRIPT env var. Mirrors the
 * Ctrl-T view but writes incrementally as the conversation grows
 * (append-only between resets; `_reset` truncates and rewrites the
 * header on /new). Plain text — no ANSI — so `cat`/`grep`/diff work
 * without a stripping step. The log struct is opaque; all four entry
 * points are NULL-safe so callers can hold a single pointer that stays
 * NULL when HAX_TRANSCRIPT is unset.
 *
 * `_open` returns NULL if HAX_TRANSCRIPT is unset or the file can't be
 * created, after printing a diagnostic in the latter case. The header
 * (banner + system prompt + tools) is written immediately so an empty
 * conversation still yields a valid file.
 *
 * `_append` renders any items added since the last call. The caller must
 * only invoke this at "stable" points where every TOOL_CALL in the
 * vector has its matching TOOL_RESULT — i.e. after the agent's inner
 * round-trip loop has dispatched the current batch of tools. Calling it
 * mid-batch would render orphaned calls.
 *
 * `_reset` truncates the file and rewrites the header. Used by /new so
 * the log reflects the current (post-reset) conversation only.
 *
 * `_close` flushes and closes the underlying file. */
struct transcript_log;
struct transcript_log *transcript_log_open(const char *system_prompt, const struct tool_def *tools,
                                           size_t n_tools);
void transcript_log_append(struct transcript_log *log, const struct item *items, size_t n_items);
void transcript_log_reset(struct transcript_log *log, const char *system_prompt,
                          const struct tool_def *tools, size_t n_tools);
void transcript_log_close(struct transcript_log *log);

#endif /* HAX_TRANSCRIPT_H */
