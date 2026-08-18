/* SPDX-License-Identifier: MIT */
#ifndef HAX_CRED_STORE_H
#define HAX_CRED_STORE_H

#include <jansson.h>

/* hax-owned credentials: one JSON object per provider id, stored with mode 0600 in the XDG state
 * tree. Entry contents are provider-defined; the store does not interpret them. Every call reads
 * the file afresh so concurrent hax processes observe each other's updates, and every write
 * serializes against other hax processes through an advisory lock. Foreground-thread state:
 * resolve entries before spawning background work. */

/* Resolved store path for diagnostics, or NULL when no home is available. The caller frees. */
char *cred_store_file_path(void);

/* Return the owned entry for `provider_id`, or NULL when absent or unreadable. */
json_t *cred_store_get(const char *provider_id);

/* Insert or replace the entry (borrowed) with an atomic 0600 write. An existing store that no
 * longer parses is replaced rather than blocking new logins. Returns 0 on success, -1 on write
 * failure. */
int cred_store_set(const char *provider_id, json_t *entry);

/* Remove the entry. Returns 1 when removed, 0 when absent, -1 when the store is unreadable or
 * cannot be rewritten. */
int cred_store_delete(const char *provider_id);

/* Remove the entry and return it in one transaction, so the caller acts on exactly what was
 * removed (e.g. revoking its token). Same returns as cred_store_delete; on 1, `*entry_out`
 * receives the owned removed entry. */
int cred_store_take(const char *provider_id, json_t **entry_out);

enum cred_store_verdict {
    CRED_STORE_KEEP,   /* leave the store untouched */
    CRED_STORE_WRITE,  /* store the owned entry set in `*replacement` */
    CRED_STORE_REMOVE, /* delete the entry */
};

/* Receive the current entry (borrowed; NULL when absent or unreadable) and decide the store's new
 * state. For CRED_STORE_WRITE set `*replacement` to the owned entry to store; returning the
 * current entry after mutating it requires taking a reference (json_incref). */
typedef enum cred_store_verdict (*cred_store_update_fn)(json_t *entry, json_t **replacement,
                                                        void *ctx);

/* Read-modify-write one entry as a single transaction: the cross-process lock is held from read
 * to write, so the entry `update` sees cannot change underneath its decision. Returns 1 when the
 * store changed (an entry written or removed), 0 when it was kept, -1 on write failure. */
int cred_store_update(const char *provider_id, cred_store_update_fn update, void *ctx);

#endif /* HAX_CRED_STORE_H */
