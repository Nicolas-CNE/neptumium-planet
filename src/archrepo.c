/*
 * archrepo.c — Neptunium Planet: Arch sync-database reader (implementation).
 *
 * See archrepo.h for the format documentation.  The pipeline is:
 *
 *   [memory or file] --libarchive--> tar members --> "desc" entries
 *                   --in-place parser--> tag/value blocks --> package*
 *                   --repo_add()--> indexed repo ready for the encoder
 *
 * Design choices worth knowing:
 *
 *   - ONE code path: file and URL loaders slurp the whole database into
 *     memory and call archrepo_load_mem(). Sync databases are ~1.5 MB
 *     compressed / ~12 MB plain, so an in-memory image is cheap and keeps the
 *     tar walking logic in a single place.
 *
 *   - In-place parsing: the 'desc' buffer has its newlines overwritten with
 *     NULs, and every tag/value string points INTO that buffer. No per-line
 *     allocations for 15 000 packages × ~20 lines each; only the small
 *     pointer arrays are heap-allocated, and the buffer is freed right after
 *     the package is built.
 *
 *   - Tolerance: a malformed entry (missing %NAME%/%VERSION%) or a line
 *     dep_parse() chokes on must NOT abort a whole 15 000-package load — it is
 *     counted in archrepo_stats and skipped. Only structural tar failures
 *     (corrupt archive) are fatal, because then everything after is suspect.
 */
#include "archrepo.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"

/* Format into the caller's errbuf (if any) and return -1 for uniform
 * `return fail(errbuf, errlen, ...)` tails. */
static int fail(char *errbuf, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int fail(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (errbuf && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(errbuf, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ---- origin URL derivation ---------------------------------------------- */

char *archrepo_origin_of(const char *url) {
    if (!url) return NULL;
    const char *slash = strrchr(url, '/');
    if (!slash) return NULL;               /* no directory component at all */
    size_t n = (size_t)(slash - url) + 1;  /* keep the trailing '/'         */
    char  *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, url, n);
    out[n] = '\0';
    return out;
}

/* ---- 'desc' tag/value block collection ---------------------------------- */

/* One "%TAG%" section: the tag and its value lines (all pointing into the
 * desc buffer, which outlives the collection). */
typedef struct {
    char  *tag;   /* into buf, without the surrounding '%' */
    char **vals;  /* pointers into buf                     */
    int    n, cap;
} tblock;

typedef struct {
    tblock *b;
    int     n, cap;
} tlist;

static void tlist_free(tlist *t) {
    for (int i = 0; i < t->n; i++) free(t->b[i].vals);
    free(t->b);
}

/* Find the block for `tag`, creating it (with no values yet) when absent. */
static tblock *tlist_get(tlist *t, char *tag) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->b[i].tag, tag) == 0) return &t->b[i];
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        tblock *nb = realloc(t->b, (size_t)t->cap * sizeof *nb);
        if (!nb) return NULL;
        t->b = nb;
    }
    tblock *b = &t->b[t->n++];
    b->tag  = tag;
    b->vals = NULL;
    b->n = b->cap = 0;
    return b;
}

static int tblock_add(tblock *b, char *val) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        char **nv = realloc(b->vals, (size_t)b->cap * sizeof *nv);
        if (!nv) return 0;
        b->vals = nv;
    }
    b->vals[b->n++] = val;
    return 1;
}

/* Single-valued accessor: the first value of `tag`, or NULL. */
static const char *tget1(const tlist *t, const char *tag) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->b[i].tag, tag) == 0)
            return t->b[i].n ? t->b[i].vals[0] : NULL;
    return NULL;
}

/* Multi-valued accessor for the four list tags we care about. */
static const tblock *tget(const tlist *t, const char *tag) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->b[i].tag, tag) == 0) return &t->b[i];
    return NULL;
}

/* Split `buf` (a whole 'desc' file, `len` bytes) into tlist blocks,
 * overwriting newlines with NULs in place.
 *
 * NOTE ON THE WALK: the end of the buffer is detected by POINTER COMPARISON
 * against buf+len, NOT by "*p != 0". Blank separator lines become empty
 * strings after the NUL pass, so a *p test would wrongly treat the first
 * section separator as end-of-buffer and silently drop everything after the
 * first tag. */
static int desc_split(char *buf, size_t len, tlist *t) {
    tblock *cur = NULL; /* current tag's block, NULL between sections */
    char   *p   = buf;
    char   *end = buf + len;

    while (p < end) {
        char *line = p;
        p += strlen(p) + 1;

        if (line[0] == '%') {
            /* Tag header: strip the trailing '%'. */
            size_t ln = strlen(line);
            if (ln >= 2 && line[ln - 1] == '%') line[ln - 1] = '\0';
            cur = tlist_get(t, line + 1);
            if (!cur) return -1;
        } else if (line[0] == '\0') {
            cur = NULL; /* blank line: end of this section */
        } else if (cur) {
            if (!tblock_add(cur, line)) return -1;
        }
        /* Lines outside any section (shouldn't happen) are ignored. */
    }
    return 0;
}

/* Normalise line endings in place: '\r\n' -> '\0\0' (the '\n' pass turns the
 * buffer into a sequence of NUL-terminated lines either way). */
static void desc_normalize(char *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n') buf[i] = '\0';
}

/* ---- one 'desc' buffer -> one package ----------------------------------- */

/* Append every value of a list tag through `add`. Returns the number of
 * values that failed to parse (baddeps accounting). */
static int add_all(package *p, const tlist *t, const char *tag,
                   int (*add)(package *, const char *)) {
    const tblock *b = tget(t, tag);
    if (!b) return 0;
    int bad = 0;
    for (int i = 0; i < b->n; i++)
        if (!add(p, b->vals[i])) bad++;
    return bad;
}

/* Parse a size tag ("%CSIZE%" / "%ISIZE%") defensively; -1 when absent or
 * non-numeric. Real databases always carry digits, but a hand-edited db must
 * not crash the loader. */
static long tget_size(const tlist *t, const char *tag) {
    const char *v = tget1(t, tag);
    if (!v || !*v) return -1;
    char *end = NULL;
    long  val = strtol(v, &end, 10);
    return (end && *end == '\0' && val >= 0) ? val : -1;
}

/* Build the package for one desc buffer and (maybe) add it to the repo.
 * Returns 0 on success; increments st->malformed for unusable entries. */
static int parse_desc(repo *r, char *buf, size_t len, const char *origin,
                      archrepo_stats *st) {
    tlist t = {NULL, 0, 0};
    int   rc = 0;

    desc_normalize(buf, len);
    if (desc_split(buf, len, &t) != 0) { tlist_free(&t); return -1; } /* OOM */

    const char *name = tget1(&t, "NAME");
    const char *ver  = tget1(&t, "VERSION");
    if (!name || !*name || !ver || !*ver) {
        st->malformed++;
        tlist_free(&t);
        return 0;
    }

    package *p = pkg_new(name, ver, tget1(&t, "ARCH"));
    if (!p) { tlist_free(&t); return -1; } /* OOM */

    /* One-line description for `planet search`. */
    pkg_set_desc(p, tget1(&t, "DESC"));

    /* Runtime dependency edges — the ones the solver reasons about. */
    st->baddeps += add_all(p, &t, "DEPENDS",   pkg_add_dep);
    st->baddeps += add_all(p, &t, "CONFLICTS", pkg_add_conflict);
    st->baddeps += add_all(p, &t, "PROVIDES",  pkg_add_provide);
    st->baddeps += add_all(p, &t, "REPLACES",  pkg_add_replace);

    /* Download metadata (see package struct in pkg.h). */
    pkg_set_meta(p, tget1(&t, "FILENAME"), tget_size(&t, "CSIZE"),
                 tget1(&t, "SHA256SUM"), origin);
    /* ISIZE is informational too; keep it in isize for --solver-stats. */
    p->isize = tget_size(&t, "ISIZE");

    /* Deduplicate across databases: the same name+version appearing twice
     * (e.g. a stale cached db merged with a fresh one) would create two
     * candidates with identical content and confuse the AMO ladder. */
    if (repo_find(r, p->name, p->version)) {
        st->duplicates++;
        pkg_free(p);
    } else {
        if (!repo_add(r, p)) { pkg_free(p); rc = -1; }
        else                  st->packages++;
    }

    tlist_free(&t);
    return rc;
}

/* ---- tar walking --------------------------------------------------------- */

/* Feed the whole archive image to the desc parser. Returns 0/-1. */
static int walk_tar(repo *r, const unsigned char *db, size_t db_len,
                    const char *origin, archrepo_stats *st,
                    char *errbuf, size_t errlen) {
    struct archive *a  = archive_read_new();
    if (!a) return fail(errbuf, errlen, "archive_read_new failed");

    /* Accept every compression filter (gzip for .db.tar.gz today, zstd for a
     * future .db.tar.zst) and plain tar. */
    archive_read_support_filter_all(a);
    archive_read_support_format_tar(a);

    if (archive_read_open_memory(a, db, db_len) != ARCHIVE_OK) {
        int rc = fail(errbuf, errlen, "cannot read database image: %s",
                      archive_error_string(a));
        archive_read_free(a);
        return rc;
    }

    for (;;) {
        struct archive_entry *e;
        int rc = archive_read_next_header(a, &e);
        if (rc == ARCHIVE_EOF) break;
        if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
            int r2 = fail(errbuf, errlen, "corrupt database tar: %s",
                          archive_error_string(a));
            archive_read_free(a);
            return r2;
        }

        /* We only want regular files named exactly "desc" (each package
         * directory holds "desc" + "files"; "files" arrives later for the
         * install path and is skipped here). */
        if (archive_entry_filetype(e) != AE_IFREG) continue;
        const char *path = archive_entry_pathname(e);
        const char *base = path ? strrchr(path, '/') : NULL;
        base = base ? base + 1 : path;
        if (!base || strcmp(base, "desc") != 0) continue;

        la_int64_t sz = archive_entry_size(e);
        if (sz <= 0 || sz > (1 << 20)) continue; /* desc files are ~1-4 KB */

        char *buf = malloc((size_t)sz + 1);
        if (!buf) {
            int r2 = fail(errbuf, errlen, "out of memory reading %s", path);
            archive_read_free(a);
            return r2;
        }
        la_ssize_t got = archive_read_data(a, buf, (size_t)sz);
        if (got < 0 || (size_t)got != (size_t)sz) {
            free(buf);
            int r2 = fail(errbuf, errlen, "truncated entry %s: %s", path,
                          archive_error_string(a));
            archive_read_free(a);
            return r2;
        }
        buf[sz] = '\0';

        int prc = parse_desc(r, buf, (size_t)sz, origin, st);
        free(buf);
        if (prc != 0) {
            int r2 = fail(errbuf, errlen, "out of memory parsing %s", path);
            archive_read_free(a);
            return r2;
        }
    }

    archive_read_free(a);
    return 0;
}

/* ---- public loaders ------------------------------------------------------ */

int archrepo_load_mem(repo *r, const void *db, size_t db_len,
                      const char *origin_url, archrepo_stats *st,
                      char *errbuf, size_t errlen) {
    archrepo_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof *st);

    if (!r || !db || db_len == 0)
        return fail(errbuf, errlen, "archrepo_load_mem: empty database");

    return walk_tar(r, (const unsigned char *)db, db_len, origin_url, st,
                    errbuf, errlen);
}

/* Read a whole file into memory (sync dbs are a few MB; see the header note
 * about the single code path). Returns 0/-1. */
static int slurp(const char *path, unsigned char **out, size_t *out_len,
                 char *errbuf, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(errbuf, errlen, "cannot open %s", path);

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return fail(errbuf, errlen, "cannot seek %s", path); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return fail(errbuf, errlen, "cannot size %s", path); }
    rewind(f);

    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return fail(errbuf, errlen, "out of memory reading %s", path); }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return fail(errbuf, errlen, "short read on %s", path); }

    *out     = buf;
    *out_len = got;
    return 0;
}

int archrepo_load_file(repo *r, const char *db_path, const char *origin_url,
                       archrepo_stats *st, char *errbuf, size_t errlen) {
    unsigned char *buf = NULL;
    size_t         len = 0;
    if (slurp(db_path, &buf, &len, errbuf, errlen) != 0) return -1;
    int rc = archrepo_load_mem(r, buf, len, origin_url, st, errbuf, errlen);
    free(buf);
    return rc;
}

int archrepo_load_url(repo *r, const char *db_url, archrepo_stats *st,
                      char *errbuf, size_t errlen) {
    unsigned char *buf = NULL;
    size_t         len = 0;

    if (net_fetch(db_url, &buf, &len, 0) != 0)
        return fail(errbuf, errlen, "%s", net_error());

    /* The download base for every package is the directory that served the
     * database: "https://h/core/os/x86_64/core.db.tar.gz" -> ".../x86_64/". */
    char *origin = archrepo_origin_of(db_url);
    int   rc     = archrepo_load_mem(r, buf, len, origin, st, errbuf, errlen);
    free(origin);
    free(buf);
    return rc;
}
