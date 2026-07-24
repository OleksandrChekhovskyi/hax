/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_TEMPFILES_H
#define HAX_SYSTEM_TEMPFILES_H

/*
 * Process-wide registry of temp files whose paths travel to the model
 * embedded in conversation history — bash output spills preserved past
 * truncation, images written on clipboard paste. The registry itself is
 * mechanism only: create-and-track plus bulk unlink. Policy — *when* the
 * tracked files become unreachable garbage — lives with the callers:
 * agent /new flushes, because it discards the history holding the
 * paths. /undo, /fork, /resume, and compaction deliberately don't:
 * histories that stay resumable in-process (a /fork's retained prefix,
 * the branch a /resume switches away from) can still reference tracked
 * files, compaction summaries are told to preserve exact paths, and
 * cleanup is all-or-nothing (see agent.c and compact.c).
 *
 * An atexit handler registered on first create flushes on process exit.
 * atexit doesn't fire on signal-driven exits (Ctrl-C, SIGHUP, kill), so
 * a signalled session can still leak; the OS eventually evicts /tmp /
 * /var/folders. Files never outlive the process, so a resumed session's
 * history may hold paths that are gone — a read of a missing path is a
 * recoverable tool error the model re-runs past, which is cheaper than
 * rewriting history on load (that would break prompt-cache reuse).
 *
 * Single-threaded by design (all callers run on the agent loop), so no
 * locking.
 */

/* Create and track a temp file (mode 0600, O_RDWR) inside a lazily
 * created private per-process 0700 directory under $TMPDIR; names are
 * prefix + sequence number + suffix, e.g.
 * tempfile_create("paste-", ".png", &p) -> /tmp/hax-Ab12Cd/paste-1.png.
 * Returns the open fd and stores the malloc'd path in *out_path (caller
 * frees), or returns -1 with *out_path NULL.
 *
 * Falls back to "/tmp" when $TMPDIR isn't valid UTF-8: the path travels
 * to the model through the provider's JSON encoder, which would mangle
 * raw bytes — we'd advertise a path that doesn't exist on disk. */
int tempfile_create(const char *prefix, const char *suffix, char **out_path);

/* Forget a tracked path without unlinking (for callers that unlink
 * early themselves, e.g. bash dropping a spill that fit within caps).
 * No-op when the path isn't tracked. */
void tempfile_untrack(const char *path);

/* Unlink and forget every tracked file, and remove the container
 * directory when that empties it. Called on /new (via the agent) and
 * from the atexit handler. */
void tempfiles_cleanup(void);

#endif /* HAX_SYSTEM_TEMPFILES_H */
