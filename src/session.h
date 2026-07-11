/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_H
#define HAX_SESSION_H

#include <jansson.h>
#include <stddef.h>

#include "provider.h" /* struct item */

/*
 * Conversation persistence: one append-only JSONL file per session, under
 * a per-cwd directory in the XDG state tree
 * ($XDG_STATE_HOME/hax/sessions/<encoded-cwd>/<ts>_<uuid>.jsonl). Line 1 is
 * a session header; every later line is one struct item, round-tripped
 * losslessly. This is a third serialization, distinct from a provider's
 * wire JSON (lossy, provider-shaped) and the HAX_TRANSCRIPT log (human
 * plain text) — the only one meant to be read back in.
 *
 * Set HAX_NO_SESSION (to any non-empty, non-"0" value) to disable
 * persistence entirely; session_log_open / _resume then return NULL and
 * all writer entry points become no-ops.
 */

/* Bumped when the on-disk item/header schema changes incompatibly. */
#define SESSION_FORMAT_VERSION 1

/* ---------------- lossless item <-> JSON codec ---------------- */

/* Serialize one item into a fresh JSON object (new reference — caller
 * json_decref). Every field present on the item is emitted; absent
 * (NULL) fields are omitted. ITEM_TURN_BOUNDARY round-trips as a
 * field-less {"kind":"turn_boundary"}. */
json_t *item_to_json(const struct item *it);

/* Parse one item object into *out (zeroed first; string fields are
 * xstrdup'd, so the caller owns them and frees via item_free). Returns 0
 * on success, -1 when `obj` is not an object or carries an unrecognized
 * (or missing) "kind". */
int item_from_json(const json_t *obj, struct item *out);

/* ---------------- session metadata (the header line) ---------------- */

struct session_meta {
    char *id;               /* uuid */
    char *cwd;              /* cwd recorded when the session began */
    char *provider;         /* HAX_PROVIDER name; may be NULL on old files */
    char *model;            /* may be NULL */
    char *reasoning_effort; /* may be NULL */
};

void session_meta_free(struct session_meta *m);

/* ---------------- append-only writer ---------------- */

struct session_log; /* opaque */

/* Begin a fresh session for the current run. provider/model/effort are
 * stamped into the header (provider drives resume-time compatibility
 * checks); the file is keyed by getcwd(). Returns NULL when sessions are
 * disabled (HAX_NO_SESSION) or no state directory is resolvable. The file
 * is created lazily on the first append, so a run that sends nothing
 * leaves no file behind. */
struct session_log *session_log_open(const char *provider, const char *model,
                                     const char *reasoning_effort);

/* Continue an existing session file: opens `path` for append and treats
 * the first `n_loaded` items as already written, so session_log_append
 * only emits items beyond them. The header is left untouched. Used by
 * both --resume and /resume so resuming continues the same file rather
 * than forking a new one. Returns NULL when sessions are disabled or the
 * file can't be opened for append. */
struct session_log *session_log_resume(const char *path, const char *provider, const char *model,
                                       const char *reasoning_effort, size_t n_loaded);

/* Append items [n_written..n_items) as one JSONL line each. No-op when
 * `log` is NULL or nothing new accumulated. Materializes the file (and
 * its parent directory + header line) on first use. */
void session_log_append(struct session_log *log, const struct item *items, size_t n_items);

/* Refresh the header provider/model/effort fields (for a runtime /provider,
 * /model, or /effort switch). Only the header line is updated — per-item
 * reasoning provenance is stamped on the items themselves — so this matters
 * only before the header is written (the lazy first append): a session that
 * starts provider-less, then selects a provider before its first prompt, ends
 * up with an accurate header instead of "none". After the header is on disk it
 * is a harmless in-memory update that the next session_log_reset carries into
 * the rotated file. No-op when `log` is NULL. */
void session_log_set_meta(struct session_log *log, const char *provider, const char *model,
                          const char *reasoning_effort);

/* Rotate to a brand-new session id/file (for /new). Closes the current
 * file; the next append lazily materializes the fresh one. */
void session_log_reset(struct session_log *log);

void session_log_close(struct session_log *log);

/* The on-disk path of the current session file (borrowed; valid until
 * reset/close). Non-NULL even before the file is materialized — it's
 * where the session will be written. NULL only when `log` is NULL or
 * sessions became unavailable after a failed reset. */
const char *session_log_path(const struct session_log *log);

/* The session id to suggest after `--resume=` to reopen this run later, or
 * NULL when nothing has been written yet (no file to resume) or `log` is
 * NULL. Resumable sessions live in the current cwd's directory, so the id
 * resolves there. Borrowed; valid until reset/close. */
const char *session_log_resume_hint(const struct session_log *log);

/* ---------------- loading & listing ---------------- */

/* Load a session file, replaying its items verbatim into a fresh malloc'd
 * vector (*out_items / *out_n) and, when out_meta is non-NULL, filling it from
 * the header.
 *
 * The load is non-destructive: model-bound reasoning (Codex's encrypted
 * reasoning_json) is kept along with its origin provider+model stamp (each
 * reasoning item carries one; older files fall back to the header). Whether a
 * blob can be replayed for a given request is decided later, by the provider's
 * build path, which compares the stamp to the current model — so a resumed
 * file may legitimately mix models, and switching back to a model still finds
 * its blobs intact.
 *
 * Returns 0 on success, -1 when the file can't be read. Caller frees the
 * items (item_free each, then free the vector) and session_meta_free. */
int session_load(const char *path, struct item **out_items, size_t *out_n,
                 struct session_meta *out_meta);

/* One row for the resume picker. */
struct session_entry {
    char *path;         /* full path to the .jsonl */
    char *id;           /* uuid, taken from the filename (may be NULL) */
    long mtime;         /* st_mtime seconds — sort + relative-time display */
    long mtime_nsec;    /* sub-second mtime — tiebreaks same-second sessions */
    char *first_prompt; /* lazily filled by the caller via session_first_prompt; NULL until then */
};

/* List sessions recorded for `cwd`, newest first (by mtime). Enumeration
 * only — stats each file and reads the id from its name, but does NOT open
 * any transcript, so --continue (newest path) and --resume=ID (id match)
 * stay cheap regardless of how many/large the sessions are. Fill
 * first_prompt lazily with session_first_prompt for the rows actually
 * shown. Returns a malloc'd array (*out / *n_out); an empty/absent
 * directory yields *out=NULL, *n_out=0. Always returns 0 (a missing
 * directory is "no sessions", not an error). Free via session_list_free. */
int session_list(const char *cwd, struct session_entry **out, size_t *n_out);

void session_list_free(struct session_entry *list, size_t n);

/* First user prompt of a session, flattened to one line and truncated to
 * `max_cells` display cells, or NULL if none / unreadable. Compaction seed
 * messages are skipped — every compacted session starts with the same
 * generic preamble, useless as a label — and a session holding only a seed
 * (compacted, no follow-up prompt within the scanned prefix) labels as
 * "(compacted)". Reads only a bounded prefix of the file (the first user
 * message is near the top), so it's cheap to call per picker row.
 * `max_cells` bounds both the cost and how far into the prompt a filter can
 * match: pass enough to search past what a row visibly shows, not just the
 * on-screen preview. Caller frees. */
char *session_first_prompt(const char *path, int max_cells);

#endif /* HAX_SESSION_H */
