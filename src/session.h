/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_H
#define HAX_SESSION_H

#include <jansson.h>
#include <stddef.h>

#include "provider.h"

/* Append-only conversation persistence. Each session is a JSONL file under the current
 * directory's bucket in the XDG state tree. The first line is a header; subsequent lines are
 * items or control records: complete provider/model/effort/preset selections, /undo cuts, and
 * completed user-turn timings. Nothing is ever rewritten, so a reader following the file sees
 * every record once and applies undo records itself. */

#define SESSION_FORMAT_VERSION 1

/* Returns a new JSON reference. NULL item fields are omitted. */
json_t *item_to_json(const struct item *item);

/* Field sources for a "type":"session" header record. All strings are borrowed; NULL fields
 * are omitted from the record. */
struct session_header {
    const char *id;
    const char *timestamp;
    const char *cwd;
    const char *provider;
    const char *model;
    const char *model_label; /* recorded only when it differs from model */
    const char *effort;
    const char *preset;
};

/* Build the header record, stamping the format and hax versions and probing the current git
 * state. Shared by the session file and the one-shot --json stream so the schema cannot
 * diverge. Returns a new reference. */
json_t *session_header_to_json(const struct session_header *header);

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

/* Prepares a fresh session for the current directory. The file is created on the first append, or
 * by session_log_begin. model_label is how the provider renders model for people; it is recorded
 * only when it differs, so a later reader shows what the banner showed without knowing any
 * provider's conventions. Returns NULL when recording is disabled or the state path cannot be
 * resolved. */
struct session_log *session_log_open(const char *provider, const char *model,
                                     const char *model_label, const char *effort,
                                     const char *preset);

/* Opens path for append. loaded_item_count is the number of items already represented in the file.
 * Pass the file's recorded selection so session_log_set_meta can detect a run-time override; a
 * label is not part of a selection, so it arrives through session_log_set_meta instead.
 * Returns NULL when recording is disabled or path cannot be opened for append. */
struct session_log *session_log_resume(const char *path, const char *provider, const char *model,
                                       const char *effort, const char *preset,
                                       size_t loaded_item_count);

/* Writes the header before any item exists, so the id is a resume handle from the start; for a
 * frontend that announces it up front. A no-op once materialized. */
void session_log_begin(struct session_log *log);

/* Appends items not previously written. All writer functions accept a NULL log. */
void session_log_append(struct session_log *log, const struct item *items, size_t item_count);

/* Updates the effective selection. Before materialization, the values update the pending header.
 * Later changes are written immediately before the next appended item, so an unused selection does
 * not alter a resumed or forked conversation. */
void session_log_set_meta(struct session_log *log, const char *provider, const char *model,
                          const char *model_label, const char *effort, const char *preset);

/* Drop a staged, unwritten selection record so the next append does not commit it. For a log
 * about to be closed or reset: a synthetic conversation-ending append must not record a
 * selection no turn has used. */
void session_log_discard_selection(struct session_log *log);

/* Closes the current file and prepares a lazily materialized session with a fresh identity. */
void session_log_reset(struct session_log *log);
void session_log_close(struct session_log *log);

/* Records that the conversation now keeps only its first keep_user_turns typed user turns. The cut
 * items stay in the file for accounting; new_item_count becomes the writer's in-memory high water
 * mark so the next append continues after the kept tail. An unmaterialized or NULL log is a
 * successful no-op. Returns -1 when the record could not be written. */
int session_log_undo(struct session_log *log, size_t keep_user_turns, size_t new_item_count);

/* Records the wall time of a completed user turn. A no-op before materialization. */
void session_log_user_turn(struct session_log *log, long elapsed_ms);

/* True after the header has been written. */
int session_log_materialized(const struct session_log *log);

/* Writes items[0, n_items) into a sibling session with a fresh identity and a forked_from header
 * field, marking every copied item inherited. The header records `selection`'s provider, model,
 * model_label, effort, and preset — the state the branch continues from, since the copied items
 * carry their own provenance — and ignores its other fields. On success, out_path receives an
 * owned path; it is NULL on failure. */
int session_fork_file(const char *source_path, const struct item *items, size_t n_items,
                      const struct session_header *selection, char **out_path);

/* Borrowed until reset or close; non-NULL before materialization when recording is available. */
const char *session_log_path(const struct session_log *log);

/* Borrowed resumable id, or NULL until the session is materialized. */
const char *session_log_resume_hint(const struct session_log *log);

/* Borrowed conversation id, fixed from open or resume until reset or close: the id a
 * materialized file carries, available before anything is written. NULL without a log. */
const char *session_log_id(const struct session_log *log);

/* True when path has hax's timestamp-and-UUID session filename. */
int session_path_is_standard(const char *path);

/* Refreshes path's mtime while coordinating with the pruner.
 * Returns 0 on success, -1 on failure. */
int session_touch(const char *path);

/* Everything a session file says about its conversation. Items and retired items are owned
 * arrays; free each item with item_free before freeing the array, or use session_loaded_free. */
struct session_loaded {
    struct item *items; /* the live conversation after applying every undo record */
    size_t n_items;
    struct item *retired; /* items undo records removed, in file order */
    size_t n_retired;
    long worked_ms;         /* sum of recorded user-turn durations */
    long last_user_turn_ms; /* newest duration recorded since the last undo; -1 when none */
    struct session_meta meta;
};

/* Loads the whole file. Invalid JSON lines are skipped, incomplete tool calls are removed from the
 * live conversation, and old reasoning items inherit header provenance. `out` is zeroed on
 * failure. */
int session_load_all(const char *path, struct session_loaded *out);

/* Releases every field still owned by `loaded`; callers that transferred an array set it NULL. */
void session_loaded_free(struct session_loaded *loaded);

/* Loads only the live conversation and optional metadata, as session_load_all does. Free items
 * with item_free followed by free, and metadata with session_meta_free. */
int session_load(const char *path, struct item **out_items, size_t *out_count,
                 struct session_meta *out_meta);

/* What a picker row can say about a session without replaying it. Every field is owned and
 * optional: old files predate the git fields, and a file may be unreadable or empty. The header
 * also records the HEAD hash, which identifies nothing to a reader and so is not surfaced. */
struct session_label {
    char *prompt; /* single-line first typed prompt, or "(compacted)" for a seed-only session */
    char *provider;
    char *model; /* the recorded display label, falling back to the wire id */
    char *effort;
    char *preset;
    char *git_branch;
    char *git_subject;
};

struct session_entry {
    char *path;
    char *id;
    long mtime;
    long mtime_nsec;
    struct session_label label; /* zeroed until populated by session_label_read */
};

/* Lists unexpired regular session files for cwd, newest first. The owned result may be empty. File
 * contents are not read; labels are populated separately. */
int session_list(const char *cwd, struct session_entry **out_entries, size_t *out_count);
void session_list_free(struct session_entry *entries, size_t count);

/* Return the owned path of cwd's prompt-history file, a sibling of its session files that listing
 * and pruning ignore. Return NULL for a NULL cwd or when no state directory is available, so an
 * unknown working directory records nothing, as with sessions. */
char *session_prompt_history_path(const char *cwd);

/* Reads a bounded file prefix, describing the session as it started: a later model or preset
 * switch is not reflected. The prompt is limited to max_cells. Overwrites out without releasing
 * it, so pass a zeroed or freed struct; unreadable files leave it zeroed. */
void session_label_read(const char *path, int max_cells, struct session_label *out);
void session_label_free(struct session_label *label);

#endif /* HAX_SESSION_H */
