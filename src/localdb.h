/*
 * localdb.h — Neptumium Planet: local (installed) package database.
 * ======================================================================
 *
 * While the repo databases tell us what IS AVAILABLE, the local database
 * records what IS INSTALLED on the target root. It lives at
 *
 *     <root>/var/lib/planet/local/<name>-<version>/{desc,files}
 *
 * in the same layout pacman uses, so the installed system is a first-class
 * citizen the solver can reason about: install/remove/upgrade all diff the
 * local db against the remote repos and hand the result to the MaxSAT
 * encoder as the system state.
 *
 * The `desc` file is a copy of the repo's `desc` (NAME/VERSION/DEPENDS/...).
 * The `files` file lists every path the package owns, one per line, which is
 * what makes remove possible without guessing.
 */
#ifndef NEPTUMIUM_LOCALDB_H
#define NEPTUMIUM_LOCALDB_H

#include "pkg.h"

/* The local db root for a given target system root. */
char *localdb_path(const char *root);

/* Mark/unmark a package as explicitly installed (vs installed as a
 * dependency). Stored as a `%EXPLICIT%` tag in its desc so the distinction
 * survives a reboot. Returns 0 on success, -1 with localdb_error() set. */
int localdb_set_explicit(const char *root, const char *name,
                         const char *version, int explicit);

/* Is this exact package (name+version) installed on `root`? */
int  localdb_is_installed(const char *root, const char *name, const char *version);

/* Full list of installed packages on `root`. Returns a malloc'd array of
 * package* (caller frees the array; packages are owned by the returned list
 * and freed with localdb_free_list). */
package **localdb_list(const char *root, int *n_out);

/* Record an installed package: writes <root>/var/lib/planet/local/<name>-<ver>/
 * desc from `p`, plus a `files` list (one path per line). Returns 0 on
 * success, -1 on failure with localdb_error() set. */
int  localdb_add(const char *root, const package *p, const char *const *files, int nfiles);

/* Remove a package's record entirely (after its files are deleted). Returns 0
 * on success, -1 on failure. */
int  localdb_remove(const char *root, const char *name, const char *version);

/* Free a list returned by localdb_list. */
void localdb_free_list(package **list);

/* Describe the most recent failure. */
const char *localdb_error(void);

#endif /* NEPTUMIUM_LOCALDB_H */
