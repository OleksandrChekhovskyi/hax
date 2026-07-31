/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_H
#define HAX_SESSION_H

#include <jansson.h>
#include <stddef.h>

#include "provider.h"

/* Append-only conversation persistence. Each session is a JSONL file under the current
 * directory's bucket in the XDG state tree. The first line is a header; subsequent lines are
 * items or complete provider/model/effort/preset selection records. */

#define SESSION_FORMAT_VERSION 1

/* Returns a new JSON reference. NULL item fields are omitted. */
json_t *item_to_json(const struct item *item);

/* Zeroes and fills out with owned fields. Free with item_free. Returns -1 for an invalid kind. */
int item_from_json(const json_t *object, struct item *out);

/* Identity and the effective selection after applying every selection record. */
struct session_meta {
    char *id;
    char *cwd;
    char *provider; /* may be NULL in old files */
    char *model;
    char *effort;
    char *preset;
};

void session_meta_free(struct session_meta *meta);

/* Reads the header and selection records without retaining items. Zeroes out on failure. */
int session_read_meta(const char *path, struct session_meta *out);

struct session_log;

/* Prepares a fresh session for the current directory. The file is created on the first append.
 * Returns NULL when recording is disabled or the state path cannot be resolved. */
struct session_log *session_log_open(const char *provider, const char *model, const char *effort,
                                     const char *preset);

/* Opens path for append. loaded_item_count is the number of items already represented in the file.
 * Pass the file's recorded selection so session_log_set_meta can detect a run-time override.
 * Returns NULL when recording is disabled or path cannot be opened for append. */
struct session_log *session_log_resume(const char *path, const char *provider, const char *model,
                                       const char *effort, const char *preset,
                                       size_t loaded_item_count);

/* Appends items not previously written. All writer functions accept a NULL log. */
void session_log_append(struct session_log *log, const struct item *items, size_t item_count);

/* Updates the effective selection. Before materialization, the values update the pending header.
 * Later changes are written immediately before the next appended item, so an unused selection does
 * not alter a resumed or forked conversation. */
void session_log_set_meta(struct session_log *log, const char *provider, const char *model,
                          const char *effort, const char *preset);

/* Closes the current file and prepares a lazily materialized session with a fresh identity. */
void session_log_reset(struct session_log *log);
void session_log_close(struct session_log *log);

/* Keeps the first keep_turns typed user turns. new_item_count becomes the writer's in-memory high
 * water mark. An unmaterialized or NULL log is a successful no-op. On failure the file is
 * unchanged. */
int session_log_truncate(struct session_log *log, size_t keep_turns, size_t new_item_count);

/* True after the header has been written. */
int session_log_materialized(const struct session_log *log);

/* Copies the first keep_turns typed user turns into a sibling session with a fresh identity and a
 * forked_from header field. On success, out_path receives an owned path; it is NULL on failure. */
int session_fork_file(const char *source_path, size_t keep_turns, char **out_path);

/* Borrowed until reset or close; non-NULL before materialization when recording is available. */
const char *session_log_path(const struct session_log *log);

/* Borrowed resumable id, or NULL until the session is materialized. */
const char *session_log_resume_hint(const struct session_log *log);

/* True when path has hax's timestamp-and-UUID session filename. */
int session_path_is_standard(const char *path);

/* Refreshes path's mtime while coordinating with the pruner.
 * Returns 0 on success, -1 on failure. */
int session_touch(const char *path);

/* Loads owned items and optional metadata. Invalid JSON lines are skipped, incomplete tool calls
 * are removed, and old reasoning items inherit header provenance. Outputs are zeroed on failure.
 * Free items with item_free followed by free, and metadata with session_meta_free. */
int session_load(const char *path, struct item **out_items, size_t *out_count,
                 struct session_meta *out_meta);

struct session_entry {
    char *path;
    char *id;
    long mtime;
    long mtime_nsec;
    char *first_prompt; /* NULL until populated by session_first_prompt */
};

/* Lists unexpired regular session files for cwd, newest first. The owned result may be empty. File
 * contents are not read; prompt labels are populated separately. */
int session_list(const char *cwd, struct session_entry **out_entries, size_t *out_count);
void session_list_free(struct session_entry *entries, size_t count);

/* Returns an owned, single-line first typed prompt limited to max_cells, "(compacted)" for a
 * seed-only session, or NULL when no label can be read. Reads only a bounded file prefix. */
char *session_first_prompt(const char *path, int max_cells);

#endif /* HAX_SESSION_H */
