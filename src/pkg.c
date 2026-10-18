/*
 * pkg.c — Neptunium/Planet package data model.
 * ============================================
 * See pkg.h for the public contract. The heart of this file is ver_cmp(), a
 * faithful implementation of the rpm/alpm version ordering that Arch uses:
 *
 *   [epoch:]pkgver[-pkgrel]
 *
 *   - epoch  compares numerically (absent => 0). A higher epoch always wins,
 *            so "2:1.0" > "1.5".
 *   - pkgver and pkgrel each compare with rpmvercmp(): the string is walked in
 *     alternating digit/alpha segments; digit segments compare as integers
 *     (so 1.10 > 1.9), alpha segments compare lexically, a digit segment beats
 *     an alpha one at the same position, and a leading '~' sorts BELOW
 *     everything (pre-release: 1.0~rc1 < 1.0).
 *
 * We keep our own tiny xstrdup/xstrndup so this file builds under plain
 * -std=c11 without feature-test macros for POSIX strdup().
 */
#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- tiny allocation helpers -------------------------------------------- */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static char *xstrndup(const char *s, size_t n) {
    if (!s) return NULL;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void substr_copy(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- version comparison -------------------------------------------------- */

/* rpm/alpm-style comparison of a single version COMPONENT (pkgver or pkgrel).
 * See the file header for the rules. */
static int rpmvercmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    if (strcmp(a, b) == 0) return 0;

    while (*a || *b) {
        /* Skip separators: anything not alphanumeric and not '~'. */
        while (*a && !isalnum((unsigned char)*a) && *a != '~') a++;
        while (*b && !isalnum((unsigned char)*b) && *b != '~') b++;

        /* '~' marks a pre-release and sorts below everything, including the
         * empty end-of-string. So 1.0~rc1 < 1.0 and 1.0~rc1 < 1.0~rc2. */
        if (*a == '~' || *b == '~') {
            if (*a != '~') return  1;   /* a has no '~'  -> a is newer */
            if (*b != '~') return -1;   /* b has no '~'  -> b is newer */
            a++; b++;
            continue;
        }
        if (!*a || !*b) break;          /* one side ran out of segments */

        if (isdigit((unsigned char)*a)) {
            /* Numeric segment: compare as integers (leading zeros are
             * irrelevant). A numeric segment outranks an alpha one. */
            if (!isdigit((unsigned char)*b)) return 1;
            char *end;
            long na = strtol(a, &end, 10); a = end;
            long nb = strtol(b, &end, 10); b = end;
            if (na != nb) return na < nb ? -1 : 1;
        } else {
            /* Alpha segment: compare lexically, char by char. */
            if (isdigit((unsigned char)*b)) return -1;   /* alpha < numeric */
            while (isalpha((unsigned char)*a) && isalpha((unsigned char)*b)) {
                if (*a != *b)
                    return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
                a++; b++;
            }
            if (isalpha((unsigned char)*a)) return  1;   /* a longer -> newer */
            if (isalpha((unsigned char)*b)) return -1;
        }
    }
    /* Whoever still has trailing content is newer; both empty means equal. */
    if (*a) return  1;
    if (*b) return -1;
    return 0;
}

/* Copy the substring [from, from+len) of the version into dst (bounded). */
static void slice(char *dst, size_t cap, const char *from, size_t len) {
    substr_copy(dst, cap, from, len);
}

int ver_cmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";

    /* ---- epoch (digits up to ':'), default 0 ---- */
    const char *ca = strchr(a, ':');
    const char *cb = strchr(b, ':');
    unsigned long ea = ca ? strtoul(a, NULL, 10) : 0UL;
    unsigned long eb = cb ? strtoul(b, NULL, 10) : 0UL;
    if (ea != eb) return ea < eb ? -1 : 1;

    const char *va = ca ? ca + 1 : a;   /* start of pkgver[-pkgrel] */
    const char *vb = cb ? cb + 1 : b;

    /* ---- split pkgver / pkgrel at the LAST '-' ---- */
    const char *da = strrchr(va, '-');
    const char *db = strrchr(vb, '-');
    const char *rela = NULL, *relb = NULL;
    size_t verlen_a, verlen_b;
    if (da) { verlen_a = (size_t)(da - va); rela = da + 1; }
    else    { verlen_a = strlen(va); }
    if (db) { verlen_b = (size_t)(db - vb); relb = db + 1; }
    else    { verlen_b = strlen(vb); }

    char vera[256], verb[256], rla[256], rlb[256];
    slice(vera, sizeof vera, va, verlen_a);
    slice(verb, sizeof verb, vb, verlen_b);
    if (rela) slice(rla, sizeof rla, rela, strlen(rela)); else rla[0] = '\0';
    if (relb) slice(rlb, sizeof rlb, relb, strlen(relb)); else rlb[0] = '\0';

    int c = rpmvercmp(vera, verb);
    if (c) return c;
    return rpmvercmp(rla, rlb);
}

int ver_satisfies(const char *ver, dep_op op, const char *constraint) {
    if (op == DEP_ANY || !constraint) return 1;   /* unconstrained */
    if (!ver) return 0;
    int c = ver_cmp(ver, constraint);
    switch (op) {
        case DEP_EQ: return c == 0;
        case DEP_GE: return c >= 0;
        case DEP_GT: return c >  0;
        case DEP_LE: return c <= 0;
        case DEP_LT: return c <  0;
        default:     return 1;
    }
}

const char *dep_op_str(dep_op op) {
    switch (op) {
        case DEP_EQ: return "=";
        case DEP_GE: return ">=";
        case DEP_GT: return ">";
        case DEP_LE: return "<=";
        case DEP_LT: return "<";
        default:     return "";
    }
}

/* ---- dependency spec ----------------------------------------------------- */

int dep_parse(dep *d, const char *spec) {
    if (!d || !spec || !*spec) return 0;
    d->name = NULL; d->version = NULL; d->op = DEP_ANY;

    /* Everything before the first operator char is the name. */
    const char *p = spec;
    while (*p && !strchr("><=", *p)) p++;
    size_t nlen = (size_t)(p - spec);
    if (nlen == 0) return 0;                     /* "" or "=1.0" — no name */

    d->name = xstrndup(spec, nlen);
    if (!d->name) return 0;
    if (*p == '\0') return 1;                    /* unversioned: "glibc" */

    dep_op op; size_t oplen;
    if      (p[0] == '>' && p[1] == '=') { op = DEP_GE; oplen = 2; }
    else if (p[0] == '<' && p[1] == '=') { op = DEP_LE; oplen = 2; }
    else if (p[0] == '>')                { op = DEP_GT; oplen = 1; }
    else if (p[0] == '<')                { op = DEP_LT; oplen = 1; }
    else if (p[0] == '=')                { op = DEP_EQ; oplen = 1; }
    else { free(d->name); d->name = NULL; return 0; }

    const char *v = p + oplen;
    if (!*v) { free(d->name); d->name = NULL; return 0; }   /* "foo>=" */

    d->version = xstrdup(v);
    if (!d->version) { free(d->name); d->name = NULL; return 0; }
    d->op = op;
    return 1;
}

void dep_free(dep *d) {
    if (!d) return;
    free(d->name);
    free(d->version);
    d->name = NULL; d->version = NULL;
}

char *dep_str(const dep *d) {
    if (!d || !d->name) return NULL;
    const char *op = dep_op_str(d->op);
    size_t n = strlen(d->name) + strlen(op) + (d->version ? strlen(d->version) : 0) + 1;
    char *s = malloc(n);
    if (!s) return NULL;
    snprintf(s, n, "%s%s%s", d->name, op, d->version ? d->version : "");
    return s;
}

int dep_matches(const dep *d, const char *version) {
    if (!d) return 0;
    return ver_satisfies(version, d->op, d->version);
}

/* ---- package ------------------------------------------------------------- */

package *pkg_new(const char *name, const char *version, const char *arch) {
    if (!name || !version) return NULL;
    package *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->name    = xstrdup(name);
    p->version = xstrdup(version);
    p->arch    = xstrdup(arch ? arch : "x86_64");
    if (!p->name || !p->version || !p->arch) { pkg_free(p); return NULL; }
    return p;
}

void pkg_free(package *p) {
    if (!p) return;
    free(p->name); free(p->version); free(p->arch); free(p->desc);
    free(p->filename); free(p->sha256); free(p->origin);
    for (int i = 0; i < p->ndepends;   i++) dep_free(&p->depends[i]);
    for (int i = 0; i < p->nconflicts; i++) dep_free(&p->conflicts[i]);
    for (int i = 0; i < p->nprovides;  i++) dep_free(&p->provides[i]);
    for (int i = 0; i < p->nreplaces;  i++) dep_free(&p->replaces[i]);
    free(p->depends); free(p->conflicts); free(p->provides); free(p->replaces);
    free(p);
}

/* Attach the download metadata a repo database provides for this package.
 * Any of the string arguments may be NULL (field left unset); csize<0 means
 * "unknown". Returns 1 on success (fields are optional: a failed strdup is
 * simply reflected as a NULL field, not worth aborting a 15k-package load). */
int pkg_set_meta(package *p, const char *filename, long csize,
                 const char *sha256, const char *origin) {
    if (!p) return 0;
    free(p->filename); p->filename = filename ? xstrdup(filename) : NULL;
    free(p->sha256);   p->sha256   = sha256   ? xstrdup(sha256)   : NULL;
    free(p->origin);   p->origin   = origin   ? xstrdup(origin)   : NULL;
    p->csize = csize;
    return 1;
}

int pkg_set_desc(package *p, const char *desc) {
    if (!p) return 0;
    free(p->desc);
    p->desc = desc ? xstrdup(desc) : NULL;
    return 1;
}

/* Parse `spec` and append it to the dynamic dep array (*arr,*n). */
static int deps_append(dep **arr, int *n, const char *spec) {
    dep d;
    if (!dep_parse(&d, spec)) return 0;
    dep *narr = realloc(*arr, (size_t)(*n + 1) * sizeof *narr);
    if (!narr) { dep_free(&d); return 0; }
    *arr = narr;
    (*arr)[*n] = d;
    (*n)++;
    return 1;
}

int pkg_add_dep(package *p, const char *spec)     { return p ? deps_append(&p->depends,   &p->ndepends,   spec) : 0; }
int pkg_add_conflict(package *p, const char *spec){ return p ? deps_append(&p->conflicts, &p->nconflicts, spec) : 0; }
int pkg_add_provide(package *p, const char *spec) { return p ? deps_append(&p->provides,  &p->nprovides,  spec) : 0; }
int pkg_add_replace(package *p, const char *spec) { return p ? deps_append(&p->replaces,  &p->nreplaces,  spec) : 0; }

int pkg_provides(const package *p, const char *name, dep_op op, const char *ver) {
    if (!p || !name) return 0;
    /* A package always satisfies a dependency on its own exact name+version. */
    if (strcmp(p->name, name) == 0 && ver_satisfies(p->version, op, ver))
        return 1;
    /* Otherwise check the extra Provides (virtual) entries. */
    for (int i = 0; i < p->nprovides; i++) {
        const dep *pv = &p->provides[i];
        if (strcmp(pv->name, name) != 0) continue;
        if (op == DEP_ANY) return 1;                 /* unversioned dep: any */
        if (pv->version && ver_satisfies(pv->version, op, ver)) return 1;
        /* An unversioned provide ("provides foo") cannot discharge a
         * versioned constraint ("foo>=2"); keep scanning other entries. */
    }
    return 0;
}

/* ---- repository ---------------------------------------------------------- */

/* The repo keeps a hash index from a *name* (own name AND every Provides
 * virtual) to the list of candidates mentioning it. A plain array would make
 * every dependency lookup O(total packages): with a real Arch repo (~15 300
 * packages) and the MaxSAT encoder calling repo_lookup() once per dependency
 * edge (~10 per package), that is ~150 000 scans × 15 300 comparisons ≈ 2×10⁹
 * pkg_provides() calls — minutes of overhead before the solver starts. With
 * the index, a lookup only touches candidates that can possibly match the
 * name; pkg_provides() then does the version filtering on that small set.
 * The index is maintained incrementally in repo_add (no removals exist). */
struct repo_bucket {
    const package **pkgs;  /* pointers into r->pkgs (not owned)          */
    int n, cap;
};

struct repo {
    package **pkgs;        /* dynamic array of owned candidates          */
    int n, cap;
    struct repo_bucket *buckets; /* name -> candidates mentioning it      */
    int nbuckets;          /* power of two, so hashing masks instead of % */
};

/* FNV-1a: tiny, good dispersion for package names, no dependencies. */
static unsigned long name_hash(const char *s) {
    unsigned long h = 1469598103934665603UL; /* FNV offset basis */
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211UL;                /* FNV prime */
    }
    return h;
}

/* Append p to the bucket of `name` (allocating entries as needed).
 * Internal; called from repo_add and repo_rehash. */
static int repo_index_one(repo *r, const package *p, const char *name) {
    struct repo_bucket *b = &r->buckets[name_hash(name) & (r->nbuckets - 1)];
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        const package **np = realloc(b->pkgs, (size_t)b->cap * sizeof *np);
        if (!np) return 0;
        b->pkgs = np;
    }
    b->pkgs[b->n++] = p;
    return 1;
}

/* Grow the bucket array when the load factor (~1 entry per bucket) is
 * exceeded, relinking every existing candidate into the fresh table. */
static int repo_rehash(repo *r) {
    if (r->nbuckets >= (r->n + 1) * 4 / 3) return 1; /* load factor ok */
    int want = r->nbuckets ? r->nbuckets * 2 : 65536;
    struct repo_bucket *nb = calloc((size_t)want, sizeof *nb);
    if (!nb) return 0;
    free(r->buckets);
    r->buckets  = nb;
    r->nbuckets = want;
    for (int i = 0; i < r->n; i++) {
        const package *p = r->pkgs[i];
        if (!repo_index_one(r, p, p->name)) return 0;
        for (int j = 0; j < p->nprovides; j++)
            if (!repo_index_one(r, p, p->provides[j].name)) return 0;
    }
    return 1;
}

repo *repo_new(void) {
    repo *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->nbuckets = 65536;
    r->buckets  = calloc((size_t)r->nbuckets, sizeof *r->buckets);
    if (!r->buckets) { free(r); return NULL; }
    return r;
}

void repo_free(repo *r) {
    if (!r) return;
    for (int i = 0; i < r->n; i++) pkg_free(r->pkgs[i]);
    for (int i = 0; i < r->nbuckets; i++) free(r->buckets[i].pkgs);
    free(r->buckets);
    free(r->pkgs);
    free(r);
}

int repo_add(repo *r, package *p) {
    if (!r || !p) return 0;
    if (!repo_rehash(r)) return 0;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        package **np = realloc(r->pkgs, (size_t)r->cap * sizeof *np);
        if (!np) return 0;
        r->pkgs = np;
    }
    r->pkgs[r->n++] = p;
    /* Link into the index: own name, then every provide virtual. */
    if (!repo_index_one(r, p, p->name)) return 0;
    for (int j = 0; j < p->nprovides; j++)
        if (!repo_index_one(r, p, p->provides[j].name)) return 0;
    return 1;
}

int repo_count(const repo *r)        { return r ? r->n : 0; }
const package *repo_get(const repo *r, int idx) {
    return (r && idx >= 0 && idx < r->n) ? r->pkgs[idx] : NULL;
}

int repo_index_of(const repo *r, const package *p) {
    if (!r || !p) return -1;
    /* repo_lookup() hands back pointers straight out of r->pkgs, so pointer
     * identity is an exact index. Callers that receive a package from a lookup
     * and need to address its slot in the repo (the encoder's reachability
     * set, for instance) use this rather than a name comparison. */
    for (int i = 0; i < r->n; i++) if (r->pkgs[i] == p) return i;
    return -1;
}

/* The bucket of candidates that could possibly satisfy `name` (their own name
 * or a provide equals it), or NULL when the name is unknown. */
static const struct repo_bucket *repo_bucket_of(const repo *r,
                                                const char *name) {
    if (!r || !name || !r->nbuckets) return NULL;
    return &r->buckets[name_hash(name) & (r->nbuckets - 1)];
}

int repo_lookup(const repo *r, const char *name, dep_op op, const char *ver,
                const package **out, int cap) {
    const struct repo_bucket *b = repo_bucket_of(r, name);
    if (!b) return 0;
    int found = 0;
    for (int i = 0; i < b->n; i++) {
        if (!pkg_provides(b->pkgs[i], name, op, ver)) continue;
        if (out && found < cap) out[found] = b->pkgs[i];
        found++;
    }
    return found;
}

int repo_lookup_count(const repo *r, const char *name, dep_op op, const char *ver) {
    return repo_lookup(r, name, op, ver, NULL, 0);
}

int repo_has_provider(const repo *r, const char *name, dep_op op, const char *ver) {
    return repo_lookup_count(r, name, op, ver) > 0;
}

const package *repo_newest(repo *r, const char *name, dep_op op, const char *ver) {
    const struct repo_bucket *b = repo_bucket_of(r, name);
    if (!b) return NULL;
    const package *best = NULL;
    for (int i = 0; i < b->n; i++) {
        const package *c = b->pkgs[i];
        if (!pkg_provides(c, name, op, ver)) continue;
        if (!best || ver_cmp(c->version, best->version) > 0) best = c;
    }
    return best;
}

const package *repo_find(const repo *r, const char *name, const char *version) {
    const struct repo_bucket *b = repo_bucket_of(r, name);
    if (!b) return NULL;
    for (int i = 0; i < b->n; i++) {
        const package *c = b->pkgs[i];
        /* Exact match only on the package's OWN name (a provider sharing the
         * name via a virtual must not be confused with the real package). */
        if (strcmp(c->name, name) == 0 && strcmp(c->version, version) == 0)
            return c;
    }
    return NULL;
}
