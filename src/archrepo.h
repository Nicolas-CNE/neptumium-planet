/*
 * archrepo.h — Neptumium Planet: Arch sync-database reader.
 *
 * WHAT THIS IS
 *   Converts an Arch sync database (core.db.tar.gz / extra.db.tar.gz) into
 *   Planet's package model: it parses a downloaded database into `package`
 *   candidates (repo*, package* from pkg.h), ready to be handed to the MaxSAT
 *   encoder.
 *
 * THE ARCH DATABASE FORMAT (what we parse here)
 *   A .db.tar.gz is a plain gzip'd tar whose members are directories, one per
 *   package, named "<name>-<pkgver>-<pkgrel>", each containing a few files:
 *
 *       bash-5.2.037-1/desc      <- metadata (the only file parsed here)
 *       bash-5.2.037-1/files     <- file list (needed later for install)
 *
 *   'desc' itself is a blank-line-separated list of "%TAG%" headers followed
 *   by one or more value lines:
 *
 *       %NAME%
 *       bash
 *
 *       %VERSION%
 *       5.2.037-1
 *
 *       %DEPENDS%
 *       readline
 *       ncurses
 *
 *   The directory name is NOT trusted: %NAME%/%VERSION% are authoritative
 *   (a directory name cannot represent an epoch such as "1:1.37.2-3"
 *   unambiguously).
 *
 *   Tags we consume: NAME VERSION ARCH DESC FILENAME CSIZE ISIZE SHA256SUM
 *   (single-valued) and DEPENDS CONFLICTS PROVIDES REPLACES (multi-valued).
 *   Tags we deliberately ignore:
 *     - MAKEDEPENDS / CHECKDEPENDS: build-time only; the binary package never
 *       needs them at install time, and encoding them would make most
 *       packages uninstallable (they reference -base-devel members).
 *     - OPTDEPENDS: informational ("bash-completion: tab completion for
 *       bash"); its value lines carry a "name: description" shape that is not
 *       a dependency. A future `planet query --why` could show them.
 *     - GROUPS LICENSE URL PACKAGER BUILDDATE MD5SUM PGPSIG: display-only or
 *       redundant with SHA256SUM.
 *
 * ORIGIN URLS
 *   Packages are downloaded from the same directory that served the database.
 *   archrepo_origin_of() derives "https://host/core/os/x86_64/" from
 *   "https://host/core/os/x86_64/core.db.tar.gz"; the loader stamps it on
 *   every package so a merged core+extra repo still fetches each package from
 *   its own mirror directory.
 */
#ifndef NEPTUMIUM_ARCHREPO_H
#define NEPTUMIUM_ARCHREPO_H

#include <stddef.h>

#include "pkg.h"

/* Per-load counters, purely for the CLI's progress/verbose output and for
 * tests (all fields zero-initialised by the loader if stats != NULL). */
typedef struct {
    int packages;    /* candidates added to the repo                     */
    int duplicates;  /* entries skipped: same name+version already there */
    int malformed;   /* 'desc' entries lacking %NAME% or %VERSION%       */
    int baddeps;     /* dependency lines dep_parse() could not parse     */
} archrepo_stats;

/* Derive the mirror directory URL from a database URL. Returns a malloc'd
 * string ("https://host/a/b/" for "https://host/a/b/core.db.tar.gz"), or NULL
 * on OOM. Also works for filesystem paths. */
char *archrepo_origin_of(const char *url);

/* Parse an in-memory database image and append every package to `r`.
 * `origin_url` (may be NULL) is stamped on each package as its download base.
 * Returns 0 on success, -1 on failure with a message in errbuf (if given).
 * Individual bad entries never fail the load: they are counted in `st`. */
int archrepo_load_mem(repo *r, const void *db, size_t db_len,
                      const char *origin_url, archrepo_stats *st,
                      char *errbuf, size_t errlen);

/* Load from a local file (used by tests and by `--offline` / cached dbs). */
int archrepo_load_file(repo *r, const char *db_path, const char *origin_url,
                       archrepo_stats *st, char *errbuf, size_t errlen);

/* Fetch over HTTP(S) via net.c and parse. Caller must have called
 * net_global_init() once at startup. */
int archrepo_load_url(repo *r, const char *db_url, archrepo_stats *st,
                      char *errbuf, size_t errlen);

#endif /* NEPTUMIUM_ARCHREPO_H */
