/*
 * localdb.c — Neptunium Planet: local (installed) package database.
 * ======================================================================
 *
 * See localdb.h for the contract. The on-disk layout mirrors pacman:
 *
 *     <root>/var/lib/planet/local/<name>-<version>/desc    (metadata copy)
 *     <root>/var/lib/planet/local/<name>-<version>/files   (owned paths)
 *
 * Keeping the same shape means the installed system is just another repo the
 * solver can diff against, and means tools that already understand pacman's
 * local db can inspect a Neptunium system.
 */
#define _GNU_SOURCE  /* strdup() under -std=c11 */

#include "localdb.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_err[512];

const char *localdb_error(void) { return g_err; }

static int fail(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

/* ---- path helpers --------------------------------------------------------- */

char *localdb_path(const char *root) {
    size_t n = strlen(root) + 64;
    char *p = malloc(n);
    if (!p) return NULL;
    /* Avoid a double slash when root is "/". */
    const char *sep = (root[0] == '/' && root[1] == '\0') ? "" : "/";
    snprintf(p, n, "%s%svar/lib/planet/local", root, sep);
    return p;
}

/* <root>/var/lib/planet/local/<name>-<version> */
static char *pkg_dir(const char *root, const char *name, const char *version) {
    char *base = localdb_path(root);
    if (!base) return NULL;
    size_t n = strlen(base) + strlen(name) + strlen(version) + 8;
    char *p = malloc(n);
    if (!p) { free(base); return NULL; }
    snprintf(p, n, "%s/%s-%s", base, name, version);
    free(base);
    return p;
}

static int mkdirs(const char *path) {
    /* mkdir -p: create each component. */
    char *tmp = strdup(path);
    if (!tmp) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    int rc = mkdir(tmp, 0755);
    if (rc != 0 && errno != EEXIST) { free(tmp); return -1; }
    free(tmp);
    return 0;
}

/* Parse the desc file in a package directory into a package*. The desc format
 * is the same as the repo databases (see archrepo.c), so we reuse the same
 * tag-scanning logic inline (kept small: we only need NAME/VERSION/DEPENDS/
 * PROVIDES/CONFLICTS/REPLACES). */
static package *parse_desc_file(const char *path, char *errbuf, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errbuf) snprintf(errbuf, errlen, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    /* Split into NUL-terminated lines in place. */
    for (size_t i = 0; i < got; i++)
        if (buf[i] == '\n') buf[i] = '\0';

    const char *name = NULL, *ver = NULL, *arch = NULL, *desc = NULL;
    int explicit_flag = 0;   /* set when the desc carries an %EXPLICIT% tag */
    dep depends[64];   int ndep = 0;
    dep provides[64];  int nprov = 0;
    dep conflicts[64]; int nconf = 0;
    dep replaces[64];  int nrepl = 0;
    char *cur = NULL;
    char *p = buf;
    char *end = buf + got;
    while (p < end) {
        char *line = p;
        p += strlen(p) + 1;
        if (line[0] == '%') {
            size_t ln = strlen(line);
            if (ln >= 2 && line[ln - 1] == '%') line[ln - 1] = '\0';
            cur = line + 1;
        } else if (line[0] == '\0') {
            cur = NULL;
        } else if (cur) {
            if (!strcmp(cur, "NAME") && !name) name = line;
            else if (!strcmp(cur, "VERSION") && !ver) ver = line;
            else if (!strcmp(cur, "ARCH") && !arch) arch = line;
            else if (!strcmp(cur, "DESC") && !desc) desc = line;
            else if (!strcmp(cur, "EXPLICIT")) explicit_flag = 1;
            else if (!strcmp(cur, "DEPENDS") && ndep < 64) {
                if (dep_parse(&depends[ndep], line)) ndep++;
            } else if (!strcmp(cur, "PROVIDES") && nprov < 64) {
                if (dep_parse(&provides[nprov], line)) nprov++;
            } else if (!strcmp(cur, "CONFLICTS") && nconf < 64) {
                if (dep_parse(&conflicts[nconf], line)) nconf++;
            } else if (!strcmp(cur, "REPLACES") && nrepl < 64) {
                if (dep_parse(&replaces[nrepl], line)) nrepl++;
            }
        }
    }
    if (!name || !ver) {
        free(buf);
        if (errbuf) snprintf(errbuf, errlen, "desc in %s lacks NAME/VERSION", path);
        return NULL;
    }
    package *pk = pkg_new(name, ver, arch);
    if (!pk) { free(buf); return NULL; }
    if (desc) pkg_set_desc(pk, desc);
    pk->explicit_installed = explicit_flag;
    /* dep_str() returns a malloc'd spec that pkg_add_*() copies from, so the
     * temporary must be freed after each call. */
    for (int i = 0; i < ndep;   i++) { char *s = dep_str(&depends[i]);   if (s) { pkg_add_dep(pk, s);       free(s); } }
    for (int i = 0; i < nprov;  i++) { char *s = dep_str(&provides[i]);  if (s) { pkg_add_provide(pk, s);  free(s); } }
    for (int i = 0; i < nconf;  i++) { char *s = dep_str(&conflicts[i]); if (s) { pkg_add_conflict(pk, s); free(s); } }
    for (int i = 0; i < nrepl;  i++) { char *s = dep_str(&replaces[i]);  if (s) { pkg_add_replace(pk, s);  free(s); } }
    for (int i = 0; i < ndep;  i++) dep_free(&depends[i]);
    for (int i = 0; i < nprov; i++) dep_free(&provides[i]);
    for (int i = 0; i < nconf; i++) dep_free(&conflicts[i]);
    for (int i = 0; i < nrepl; i++) dep_free(&replaces[i]);
    free(buf);
    return pk;
}

/* ---- queries -------------------------------------------------------------- */

int localdb_is_installed(const char *root, const char *name, const char *version) {
    char *dir = pkg_dir(root, name, version);
    if (!dir) return -1;
    struct stat st;
    int rc = stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
    free(dir);
    return rc;
}

package **localdb_list(const char *root, int *n_out) {
    *n_out = 0;
    char *base = localdb_path(root);
    if (!base) return NULL;
    DIR *d = opendir(base);
    if (!d) { free(base); return NULL; }

    package **list = NULL;
    int n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *desc = malloc(strlen(base) + strlen(ent->d_name) + 16);
        if (!desc) continue;
        snprintf(desc, strlen(base) + strlen(ent->d_name) + 16,
                 "%s/%s/desc", base, ent->d_name);
        char err[256] = {0};
        package *p = parse_desc_file(desc, err, sizeof err);
        free(desc);
        if (!p) continue;
        if (n + 1 > cap) {
            /* Always keep one spare slot: the list is NULL-terminated so
             * localdb_free_list() can walk it without a length. */
            cap = cap ? cap * 2 : 16;
            package **nl = realloc(list, (size_t)cap * sizeof *nl);
            if (!nl) { pkg_free(p); continue; }
            list = nl;
        }
        list[n++] = p;
    }
    closedir(d);
    free(base);
    if (list) list[n] = NULL;
    *n_out = n;
    return list;
}

void localdb_free_list(package **list) {
    if (!list) return;
    for (int i = 0; list[i]; i++) pkg_free(list[i]);
    free(list);
}

/* ---- mutations ------------------------------------------------------------ */

int localdb_add(const char *root, const package *p,
                const char *const *files, int nfiles) {
    char *dir = pkg_dir(root, p->name, p->version);
    if (!dir) return fail("out of memory");
    if (mkdirs(dir) != 0) {
        int r = fail("cannot create %s: %s", dir, strerror(errno));
        free(dir);
        return r;
    }
    char *desc = malloc(strlen(dir) + 16);
    snprintf(desc, strlen(dir) + 16, "%s/desc", dir);
    FILE *f = fopen(desc, "w");
    if (!f) {
        int r = fail("cannot write %s: %s", desc, strerror(errno));
        free(desc); free(dir);
        return r;
    }
    fprintf(f, "%%NAME%%\n%s\n\n%%VERSION%%\n%s\n\n", p->name, p->version);
    if (p->arch)    fprintf(f, "%%ARCH%%\n%s\n\n", p->arch);
    if (p->desc)    fprintf(f, "%%DESC%%\n%s\n\n", p->desc);
    if (p->filename) fprintf(f, "%%FILENAME%%\n%s\n\n", p->filename);
    if (p->csize >= 0) fprintf(f, "%%CSIZE%%\n%ld\n\n", p->csize);
    if (p->isize >= 0) fprintf(f, "%%ISIZE%%\n%ld\n\n", p->isize);
    if (p->sha256)  fprintf(f, "%%SHA256SUM%%\n%s\n\n", p->sha256);
    if (p->origin)  fprintf(f, "%%URL%%\n%s\n\n", p->origin);
    if (p->explicit_installed) fprintf(f, "%%EXPLICIT%%\n1\n\n");
    for (int i = 0; i < p->ndepends; i++)
        fprintf(f, "%%DEPENDS%%\n%s%s%s\n", p->depends[i].name,
                dep_op_str(p->depends[i].op),
                p->depends[i].version ? p->depends[i].version : "");
    for (int i = 0; i < p->nprovides; i++)
        fprintf(f, "%%PROVIDES%%\n%s%s%s\n", p->provides[i].name,
                dep_op_str(p->provides[i].op),
                p->provides[i].version ? p->provides[i].version : "");
    for (int i = 0; i < p->nconflicts; i++)
        fprintf(f, "%%CONFLICTS%%\n%s\n", p->conflicts[i].name);
    for (int i = 0; i < p->nreplaces; i++)
        fprintf(f, "%%REPLACES%%\n%s\n", p->replaces[i].name);
    fclose(f);

    /* files list */
    char *fl = malloc(strlen(dir) + 16);
    snprintf(fl, strlen(dir) + 16, "%s/files", dir);
    f = fopen(fl, "w");
    if (!f) {
        int r = fail("cannot write %s: %s", fl, strerror(errno));
        free(fl); free(desc); free(dir);
        return r;
    }
    for (int i = 0; i < nfiles; i++)
        fprintf(f, "%s\n", files[i]);
    fclose(f);
    free(fl); free(desc); free(dir);
    return 0;
}

int localdb_set_explicit(const char *root, const char *name,
                         const char *version, int explicit) {
    char *dir = pkg_dir(root, name, version);
    if (!dir) return fail("out of memory");
    char *path = malloc(strlen(dir) + 16);
    if (!path) { free(dir); return fail("out of memory"); }
    snprintf(path, strlen(dir) + 16, "%s/desc", dir);

    /* Read the desc, drop any existing %EXPLICIT% block, re-append if wanted. */
    FILE *f = fopen(path, "r");
    if (!f) {
        int e = errno;
        fail("cannot read %s: %s", path, strerror(e));
        free(path); free(dir);
        return -1;
    }
    char *buf = NULL; size_t used = 0, cap = 0;
    char line[1024];
    int skip_block = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '%') {
            skip_block = (strncmp(line, "%EXPLICIT%", 10) == 0);
            if (skip_block) continue;
        } else if (skip_block) {
            if (line[0] == '\n' || line[0] == '\r') skip_block = 0;  /* block ended */
            else continue;
        }
        size_t l = strlen(line);
        if (used + l + 1 > cap) {
            cap = (used + l + 1) * 2;
            char *g = realloc(buf, cap);
            if (!g) { free(buf); fclose(f); free(path); free(dir); return fail("out of memory"); }
            buf = g;
        }
        memcpy(buf + used, line, l);
        used += l;
    }
    fclose(f);
    if (!buf) { buf = strdup(""); if (!buf) { free(path); free(dir); return fail("out of memory"); } used = 0; }

    if (explicit) {
        size_t l = strlen("%EXPLICIT%\n1\n\n");
        char *g = realloc(buf, used + l + 1);
        if (!g) { free(buf); free(path); free(dir); return fail("out of memory"); }
        buf = g;
        memcpy(buf + used, "%EXPLICIT%\n1\n\n", l);
        used += l;
    }

    f = fopen(path, "w");
    if (!f) {
        int e = errno;
        fail("cannot write %s: %s", path, strerror(e));
        free(buf); free(path); free(dir);
        return -1;
    }
    fwrite(buf, 1, used, f);
    fclose(f);
    free(buf); free(path); free(dir);
    return 0;
}

int localdb_remove(const char *root, const char *name, const char *version) {
    char *dir = pkg_dir(root, name, version);
    if (!dir) return fail("out of memory");
    /* Delete desc and files, then the directory. */
    char *p = malloc(strlen(dir) + 16);
    snprintf(p, strlen(dir) + 16, "%s/desc", dir);
    remove(p);
    snprintf(p, strlen(dir) + 16, "%s/files", dir);
    remove(p);
    int rc = rmdir(dir);
    if (rc != 0) {
        int r = fail("cannot remove %s: %s", dir, strerror(errno));
        free(p); free(dir);
        return r;
    }
    free(p); free(dir);
    return 0;
}
