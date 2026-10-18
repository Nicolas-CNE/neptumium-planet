/*
 * pkgfile.h — Neptunium Planet: package payload fetch, verify and extract.
 * =========================================================================
 *
 * While the repo databases tell us WHAT a package is, the payload is the
 * actual .pkg.tar.zst file. This module bridges the two:
 *
 *   1. DOWNLOAD   fetch <origin>/<filename> via the net layer into a cache
 *                 file (content-addressed by the expected sha256), retrying
 *                 transient failures.
 *   2. VERIFY     compute SHA-256 over the cached file and compare it to the
 *                 database's %SHA256SUM%. A package manager that does not
 *                 verify its inputs is a supply-chain hole; verification is
 *                 MANDATORY, never optional.
 *   3. EXTRACT    unpack the .pkg.tar.zst into a target root with libarchive
 *                 (zstd filter), restoring permissions, ownership, symlinks
 *                 and hardlinks, and skipping the metadata entries (.PKGINFO,
 *                 .MTREE, .INSTALL) that live in the payload but not on disk.
 *
 * The SHA-256 is self-contained (no OpenSSL dependency) so Planet builds on
 * a minimal rootfs and the audit trail stays tiny.
 */
#ifndef NEPTUMIUM_PKGFILE_H
#define NEPTUMIUM_PKGFILE_H

#include <stddef.h>
#include <stdio.h>

#include "pkg.h"

/* Default package cache: $XDG_CACHE_HOME/planet/pkgs, else
 * ~/.cache/planet/pkgs, else ./.planet-cache/pkgs. */
char *pkgfile_cache_dir(void);

/* Override the package cache location. The CLI passes its -c/--cache-dir here
 * so payloads are cached beside the repository databases; NULL restores the
 * default. */
void pkgfile_cache_dir_set(const char *dir);

/* Download `p` (using p->origin + p->filename) into the cache and verify it
 * against p->sha256. On success returns the path to the cached file (malloc'd,
 * caller frees). Returns NULL with a message from pkgfile_error(). */
char *pkgfile_fetch(const package *p);

/* Verify an already-downloaded file at `path` against the package's expected
 * sha256 (p->sha256). Returns 1 if it matches, 0 if not, -1 on I/O error. */
int pkgfile_verify(const package *p, const char *path);

/* Extract the payload at `path` into `root`. .PKGINFO/.MTREE/.INSTALL and the
 * other metadata entries at the archive root are skipped (they are metadata,
 * not files).
 *
 * If `files_out` is non-NULL it receives a malloc'd array of malloc'd strings
 * — every path written, in extraction order — and `nfiles_out` the count.
 * That is what the local database records as the package's file list, so a
 * later removal knows exactly which paths to delete. Free it with
 * pkgfile_free_list().
 *
 * Returns 0 on success, -1 with pkgfile_error() set. */
int pkgfile_extract(const char *path, const char *root,
                    char ***files_out, int *nfiles_out);

/* Extract just the payload's .INSTALL script to `out_path` (a path the caller
 * chooses, typically a temp file). ALPM packages ship a bash script defining
 * pre/post install/upgrade/remove functions; we pull it out so the hooks layer
 * can source it at the right moment. Returns 1 if the payload had a .INSTALL
 * script (and it was written), 0 if it did not, -1 on error. */
int pkgfile_extract_install_script(const char *pkg_path, const char *out_path);

/* Free a file list returned by pkgfile_extract(). */
void pkgfile_free_list(char **files, int n);

/* Describe the most recent failure. */
const char *pkgfile_error(void);

#endif /* NEPTUMIUM_PKGFILE_H */
