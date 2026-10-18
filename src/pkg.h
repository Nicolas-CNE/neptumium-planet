/*
 * pkg.h — Neptumium/Planet package data model.
 * ============================================
 *
 * This is the layer that turns the abstract world of "packages" into data the
 * MaxSAT encoding can chew on. It knows nothing about SAT; it only models:
 *
 *   - VERSIONS   in the exact format Arch uses:  [epoch:]pkgver[-pkgrel]
 *                with rpmvercmp-style comparison (including '~' pre-release
 *                ordering and numeric > alpha segment rules). Version
 *                comparison is a common source of subtle bugs, so it is
 *                implemented once here and covered by test_pkg.c.
 *
 *   - DEPENDENCY SPECS  like "glibc", "gcc>=12", "bash=5.2-1" — a target name
 *                plus an optional operator and version.
 *
 *   - PACKAGES   with their Depends / Conflicts / Provides / Replaces lists.
 *
 *   - A REPO     an indexed collection of *candidate* packages (every name may
 *                have several versions, and any package may provide virtual
 *                names). The encoding layer asks the repo "which candidates can
 *                satisfy dependency (name, op, ver)?" and builds clauses.
 *
 * The MaxSAT encoding (encode.h) is the only consumer that matters; it maps
 * each candidate to a boolean variable and turns the relations below into hard
 * clauses (Depends/Conflicts/Provides/Replaces) and weighted soft clauses
 * (user requests, newest-version preference, minimal footprint).
 */
#ifndef NEPTUMIUM_PKG_H
#define NEPTUMIUM_PKG_H

#include <stddef.h>

/* ---- version operators --------------------------------------------------- */

/* How a dependency constrains the version of its target. */
typedef enum {
    DEP_ANY = 0,   /* "foo"        — any version will do            */
    DEP_EQ,        /* "foo=1.2"    — exactly this version           */
    DEP_GE,        /* "foo>=1.2"   — at least 1.2                   */
    DEP_GT,        /* "foo>1.2"    — strictly greater than 1.2      */
    DEP_LE,        /* "foo<=1.2"   — at most 1.2                    */
    DEP_LT         /* "foo<1.2"    — strictly less than 1.2         */
} dep_op;

/* Human-readable operator string ("" for DEP_ANY). */
const char *dep_op_str(dep_op op);

/* ---- version comparison -------------------------------------------------- */

/* Compare two full versions "[epoch:]pkgver[-pkgrel]".
 * Returns <0 if a<b, 0 if a==b, >0 if a>b.  NULL sorts before anything
 * (treated as the empty version, which is lower than every real version). */
int ver_cmp(const char *a, const char *b);

/* Does concrete version `ver` satisfy the constraint (op, constraint_version)?
 * A DEP_ANY constraint (or a NULL/absent constraint) is satisfied by any
 * version. `constraint` may be NULL exactly when op is DEP_ANY. */
int ver_satisfies(const char *ver, dep_op op, const char *constraint);

/* ---- dependency spec ----------------------------------------------------- */

typedef struct {
    char   *name;      /* target package (or virtual) name            */
    dep_op  op;        /* version operator (DEP_ANY when unversioned) */
    char   *version;   /* constraint version; NULL iff op == DEP_ANY  */
} dep;

/* Parse a spec such as "glibc", "gcc>=12", "linux-api-headers<=6.5".
 * Returns 1 on success (d is filled with freshly allocated strings), 0 on
 * malformed input. Free with dep_free(). */
int  dep_parse(dep *d, const char *spec);
void dep_free(dep *d);

/* Rebuild a spec string ("name", "name>=1.2", "name=1.2-1") from a parsed dep.
 * Returns a malloc'd string (caller frees), or NULL on OOM. Handy for
 * round-tripping deps through files (e.g. the local database). */
char *dep_str(const dep *d);

/* Does (op,constraint) accept the given concrete version? Convenience wrapper
 * around ver_satisfies for a dep that may be unversioned. */
int dep_matches(const dep *d, const char *version);

/* ---- package ------------------------------------------------------------- */

typedef struct {
    char *name;
    char *version;      /* full "[epoch:]ver[-rel]"                    */
    char *arch;         /* "x86_64", "any", ... (informational here)   */

    dep *depends;   int ndepends;    /* hard requirements               */
    dep *conflicts; int nconflicts;  /* must NOT coexist                */
    dep *provides;  int nprovides;   /* virtual names this satisfies    */
    dep *replaces;  int nreplaces;   /* supersedes / takes over from    */

    int   installed;    /* already present in the target root?         */
    /* Explicitly requested by the user (vs pulled in as a dependency).
     * This is what makes `autoremove` correct: a package stays if it is
     * explicit OR is reachable from an explicit package through dependencies. */
    int   explicit_installed;
    long  isize;        /* installed size in bytes (for --solver-stats)*/
    char *desc;         /* one-line description (for search)           */

    /* ---- repository / download metadata (populated by the repo layer) ----
     * These come straight from the Arch sync database 'desc' entries and are
     * what turns "the solver picked this package" into "we can actually fetch
     * and verify it":
     *   filename  — e.g. "glibc-2.40-3-x86_64.pkg.tar.zst" (%FILENAME%);
     *   csize     — compressed download size in bytes (%CSIZE%), used for
     *               progress bars and bandwidth estimates;
     *   sha256    — hex digest (%SHA256SUM%) — verified byte-for-byte after
     *               download; a package manager that doesn't verify is a
     *               supply-chain hole;
     *   origin    — base URL of the mirror hosting this package (derived from
     *               the db URL, kept per-package so a mixed core/extra repo
     *               still downloads from the right place).
     * All optional: the solver never reads them; only the fetcher does. */
    char *filename;
    long  csize;
    char *sha256;
    char *origin;
} package;

package *pkg_new(const char *name, const char *version, const char *arch);
void     pkg_free(package *p);

/* Append helpers (copy the strings). Return 1 on success, 0 on OOM/bad input.
 * pkg_add_dep takes a spec string; pkg_add_dep_vo takes explicit fields. */
int pkg_add_dep(package *p, const char *spec);
int pkg_add_conflict(package *p, const char *spec);
int pkg_add_provide(package *p, const char *spec);
int pkg_add_replace(package *p, const char *spec);

/* Attach download metadata from the repo database (all args optional;
 * csize<0 = unknown). See the package struct for what each field means. */
int pkg_set_meta(package *p, const char *filename, long csize,
                 const char *sha256, const char *origin);

/* Set the one-line description (search display); NULL clears it. */
int pkg_set_desc(package *p, const char *desc);

/* Does this package provide the virtual (name, op, ver)?
 * A package always provides its own exact name+version; extra Provides entries
 * cover virtuals. */
int pkg_provides(const package *p, const char *name, dep_op op, const char *ver);

/* ---- repository ---------------------------------------------------------- */

/* A repo is an indexed bag of candidate packages. It owns the packages added
 * to it (freed by repo_free). Lookups return const pointers valid until the
 * repo changes. */
typedef struct repo repo;

repo *repo_new(void);
void  repo_free(repo *r);

/* Add a candidate (repo takes ownership of p). Returns 1 on success. */
int repo_add(repo *r, package *p);

int repo_count(const repo *r);
const package *repo_get(const repo *r, int idx);
/* Index of p within the repo, or -1. Exact: repo_lookup() returns pointers
 * straight from the repo, so pointer identity identifies the slot. */
int repo_index_of(const repo *r, const package *p);

/* All candidates that can satisfy the dependency (name, op, ver) — either by
 * matching name+version or by providing a virtual. Fills out[0..cap-1] and
 * returns the number found (may exceed cap; caller should size cap generously
 * or call repo_lookup_count first). */
int repo_lookup(const repo *r, const char *name, dep_op op, const char *ver,
                const package **out, int cap);
int repo_lookup_count(const repo *r, const char *name, dep_op op, const char *ver);

/* The newest candidate providing/matching (name, op, ver), or NULL. */
const package *repo_newest(repo *r, const char *name, dep_op op, const char *ver);

/* Exact candidate with this name AND this full version string, or NULL. Used
 * by the repo loaders to skip duplicates when several databases are merged. */
const package *repo_find(const repo *r, const char *name, const char *version);

/* Does ANY candidate satisfy (name,op,ver)? Handy for detecting a dependency
 * that can never be met (=> the dependent package is uninstallable). */
int repo_has_provider(const repo *r, const char *name, dep_op op, const char *ver);

#endif /* NEPTUMIUM_PKG_H */
