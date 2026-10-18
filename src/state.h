/*
 * state.h — Neptunium Planet: transaction locking and system profiles.
 * ======================================================================
 *
 * Two pieces of transaction safety that a package manager cannot do without:
 *
 * 1. LOCKING. Two concurrent transactions against one root interleave their
 *    local-database writes and can produce a system that neither of them
 *    intended. Planet takes an exclusive lock (<root>/var/lib/planet/db.lck)
 *    for the duration of a mutating transaction. A lock left behind by a
 *    crashed process is detected via its recorded PID and reclaimed only if
 *    that process is really gone.
 *
 * 2. PROFILES. A profile is a named snapshot of the installed set (the same
 *    content as the lock file). Because Planet computes transactions with an
 *    exact solver, "roll back to profile X" is just "solve for the set in X
 *    and apply the difference" — which also means a rollback can be validated
 *    before anything is touched, and can refuse if the old set is no longer
 *    satisfiable (a repository can drop packages over time).
 */
#ifndef NEPTUMIUM_STATE_H
#define NEPTUMIUM_STATE_H

/* Acquire the transaction lock for `root`. Returns 0 on success, -1 if another
 * live transaction holds it (message via state_error()). The lock is released
 * by lock_release() or automatically on exit(). */
int  lock_acquire(const char *root);
void lock_release(void);

/* Profiles ---------------------------------------------------------------- */

/* Write the currently installed set as profile `name`. 0 on success. */
int profile_save(const char *root, const char *name);

/* List profile names, one per line, into a malloc'd string (caller frees). */
char *profile_list(const char *root);

/* Read profile `name` into a malloc'd array of malloc'd "name=version"
 * strings (caller frees each and the array). Returns the count, or -1. */
int profile_load(const char *root, const char *name, char ***out);

/* Delete profile `name`. 0 on success, -1 if it does not exist. */
int profile_remove(const char *root, const char *name);

/* Write the installed set to the canonical lock file
 * (<root>/var/lib/planet/installed.lock). 0 on success. */
int lock_write(const char *root);

const char *state_error(void);

#endif /* NEPTUMIUM_STATE_H */