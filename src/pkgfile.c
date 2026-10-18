/*
 * pkgfile.c — Neptunium Planet: package payload fetch, verify and extract.
 * =========================================================================
 *
 * See pkgfile.h for the contract. The pipeline per package:
 *
 *   origin + filename  --net_fetch-->  cache file (or in-memory)
 *   cache file         --sha256----->  compare with p->sha256 (MANDATORY)
 *   verified payload   --libarchive--> target root (skip .PKGINFO/.MTREE/.INSTALL)
 *
 * The cache is content-addressed by the expected sha256 so a verified file
 * is never re-downloaded and a corrupted one is never reused.
 */
#define _GNU_SOURCE  /* strdup() under -std=c11 */

#include "pkgfile.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net.h"
#include "sha256.h"

static char g_err[512];

const char *pkgfile_error(void) { return g_err; }

static int fail(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

/* Pointer-returning variant: record the error and return NULL. */
static char *failp(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static char *failp(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return NULL;
}

/* ---- cache path ---------------------------------------------------------- */

/* Cache directory override: the CLI passes its -c/--cache-dir here so the
 * package cache lives beside the database cache instead of silently going to
 * a different place. NULL restores the XDG default. */
static char g_cache_override[1024];

void pkgfile_cache_dir_set(const char *dir) {
    if (!dir) { g_cache_override[0] = '\0'; return; }
    snprintf(g_cache_override, sizeof g_cache_override, "%s", dir);
}

/* mkdir -p for the cache path. */
static int mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

char *pkgfile_cache_dir(void) {
    const char *base = g_cache_override[0] ? g_cache_override : NULL;
    size_t n;
    char *p;
    if (!base) {
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) {
            n = strlen(xdg) + 32;
            p = malloc(n);
            if (p) snprintf(p, n, "%s/planet/pkgs", xdg);
            return p;
        }
        const char *home = getenv("HOME");
        if (home && *home) {
            n = strlen(home) + 40;
            p = malloc(n);
            if (p) snprintf(p, n, "%s/.cache/planet/pkgs", home);
            return p;
        }
        return strdup(".planet-cache/pkgs");
    }
    /* --cache-dir points at the directory holding the databases; payloads go
     * in its "pkgs" subdirectory. */
    size_t bl = strlen(base);
    while (bl > 1 && base[bl - 1] == '/') bl--;      /* drop trailing slash */
    n = bl + 16;
    p = malloc(n);
    if (p) snprintf(p, n, "%.*s/pkgs", (int)bl, base);
    return p;
}

/* Build <cache>/<sha256>.pkg for a package. The filename IS the digest, so a
 * cached file that verifies is definitionally the right bytes. */
static char *cache_path_for(const package *p) {
    char *dir = pkgfile_cache_dir();
    if (!dir) return NULL;
    size_t n = strlen(dir) + 80;
    char *path = malloc(n);
    if (!path) { free(dir); return NULL; }
    snprintf(path, n, "%s/%s.pkg", dir, p->sha256 ? p->sha256 : "unknown");
    free(dir);
    return path;
}

/* ---- download + verify --------------------------------------------------- */

char *pkgfile_fetch(const package *p) {
    if (!p || !p->origin || !p->filename)
        return failp("package %s has no download metadata", p ? p->name : "?");
    if (!p->sha256)
        return failp("package %s has no sha256 in the database", p->name);

    char *dir = pkgfile_cache_dir();
    if (!dir) return failp("out of memory");
    if (mkdirs(dir) != 0) {
        int e = errno;
        free(dir);
        return failp("cannot create package cache: %s", strerror(e));
    }

    char *path = cache_path_for(p);
    if (!path) { free(dir); return failp("out of memory"); }

    /* Already cached and verified? Skip the network entirely. */
    struct stat st;
    if (stat(path, &st) == 0) {
        if (pkgfile_verify(p, path) == 1) { free(dir); return path; }
        remove(path);   /* corrupt cached file: drop it and re-fetch */
    }

    size_t ulen = strlen(p->origin) + strlen(p->filename) + 2;
    char *url = malloc(ulen);
    if (!url) { free(dir); free(path); return failp("out of memory"); }
    snprintf(url, ulen, "%s%s", p->origin, p->filename);

    if (net_global_init() != 0) {
        fail("network init: %s", net_error());
        free(dir); free(url); free(path);
        return NULL;
    }

    /* Download to a temp file next to the cache path, then verify + rename.
     * A failed download never leaves a half-written cached payload behind. */
    size_t tlen = strlen(path) + 8;
    char *tmp = malloc(tlen);
    if (!tmp) {
        free(dir); free(url); free(path);
        net_global_cleanup();
        return failp("out of memory");
    }
    snprintf(tmp, tlen, "%s.tmp", path);

    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        fail("cannot create %s: %s", tmp, strerror(errno));
        free(dir); free(url); free(tmp); free(path);
        net_global_cleanup();
        return NULL;
    }
    int rc = net_fetch_to_file(url, fp, 0);
    fclose(fp);
    if (rc != 0) {
        fail("download %s: %s", url, net_error());
        remove(tmp);
        free(dir); free(url); free(tmp); free(path);
        net_global_cleanup();
        return NULL;
    }

    if (pkgfile_verify(p, tmp) != 1) {
        fail("sha256 mismatch for %s (expected %s, got %s)",
                     p->name, p->sha256, "computed");
        remove(tmp);
        free(dir); free(url); free(tmp); free(path);
        net_global_cleanup();
        return NULL;
    }

    if (rename(tmp, path) != 0) {
        fail("cannot move %s into cache: %s", tmp, strerror(errno));
        remove(tmp);
        free(dir); free(url); free(tmp); free(path);
        net_global_cleanup();
        return NULL;
    }

    free(dir); free(url); free(tmp);
    net_global_cleanup();
    return path;
}

int pkgfile_verify(const package *p, const char *path) {
    if (!p || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return fail("cannot open %s: %s", path, strerror(errno));

    sha256_ctx c;
    sha256_init(&c);
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&c, buf, got);
    fclose(f);

    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_final(&c, digest);
    char hex[65];
    sha256_hex(digest, hex);

    if (!p->sha256) return fail("no expected sha256 to compare against");
    if (strcmp(hex, p->sha256) != 0)
        return fail("%s: sha256 mismatch: expected %s, computed %s",
                    p->name ? p->name : "package", p->sha256, hex);
    return 1;
}

/* ---- extraction ---------------------------------------------------------- */

/* Effective entry name with any leading "./" stripped. Some archives (and some
 * build tooling) store members as "./usr/bin/x" rather than "usr/bin/x", and
 * metadata entries likewise as "./.INSTALL". Comparing the raw name would
 * then silently miss both the metadata filter and the .INSTALL lookup. */
static const char *entry_name(const char *raw) {
    while (raw[0] == '.' && raw[1] == '/') raw += 2;
    return raw;
}

/* Should this archive entry be written to disk? Skip the metadata entries
 * that Arch packages carry at the root (.PKGINFO, .MTREE, .INSTALL, .CHANGELOG)
 * and dot-files at the root; everything else is a real file. */
static int entry_is_metadata(const char *name) {
    const char *n = entry_name(name);
    static const char *meta[] = {".PKGINFO", ".MTREE", ".INSTALL", ".CHANGELOG", NULL};
    for (int i = 0; meta[i]; i++)
        if (strcmp(n, meta[i]) == 0) return 1;
    return 0;
}

/* Walk an opened package payload, writing it to disk through `ext` and
 * recording every path in the file list. Runs with the process already
 * chdir'd into the target root (see pkgfile_extract). Returns 0 on success,
 * -1 on error with the message already recorded. */
static int extract_loop(struct archive *a, struct archive *ext, const char *path,
                        char ***files_p, int *nfiles, int *cap) {
    char **files = *files_p;
    int rc = ARCHIVE_OK;
    struct archive_entry *e;
    while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(e);
        if (!name) continue;
        if (entry_is_metadata(name)) {
            archive_read_data_skip(a);
            continue;
        }
        archive_entry_set_pathname(e, name);
        if (archive_write_header(ext, e) != ARCHIVE_OK) {
            fail("extract %s: %s", name, archive_error_string(ext));
            pkgfile_free_list(files, *nfiles);
            return -1;
        }
        /* Record the path so removal can find it later. Directories are
         * recorded too: they must be removed after their contents. */
        if (*nfiles == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            char **nf = realloc(files, (size_t)*cap * sizeof *nf);
            if (!nf) {
                fail("out of memory recording %s", name);
                pkgfile_free_list(files, *nfiles);
                return -1;
            }
            files = nf;
            *files_p = files;   /* realloc may have moved the array */
        }
        files[*nfiles] = strdup(name);
        if (!files[*nfiles]) {
            fail("out of memory recording %s", name);
            pkgfile_free_list(files, *nfiles);
            return -1;
        }
        (*nfiles)++;

        /* Copy data for regular files; symlinks/hardlinks carry none. */
        const void *blk; size_t sz; la_int64_t off;
        while (archive_read_data_block(a, &blk, &sz, &off) == ARCHIVE_OK)
            if (archive_write_data_block(ext, blk, sz, off) < 0) {
                fail("write %s: %s", name, archive_error_string(ext));
                pkgfile_free_list(files, *nfiles);
                return -1;
            }
        archive_write_finish_entry(ext);
    }
    if (rc != ARCHIVE_EOF) {
        fail("corrupt archive %s: %s", path, archive_error_string(a));
        pkgfile_free_list(files, *nfiles);
        return -1;
    }
    return 0;
}

int pkgfile_extract(const char *path, const char *root,
                    char ***files_out, int *nfiles_out) {
    char **files = NULL;
    int    nfiles = 0, cap = 0;

    if (files_out) { *files_out = NULL; *nfiles_out = 0; }

    struct archive *a = archive_read_new();
    if (!a) return fail("archive_read_new failed");
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    struct archive *ext = archive_write_disk_new();
    if (!ext) {
        archive_read_free(a);
        return fail("archive_write_disk_new failed");
    }
    /* Ownership is only restored when we are actually root: package payloads
     * record uid/gid 0, and a non-root extraction cannot chown to it. As a
     * non-root user the files land owned by the invoking uid instead, which
     * matches a freshly unpacked test rootfs; a `chown -R 0:0` is required
     * before booting such a tree as a real system.
     *
     * SECURE_SYMLINKS is deliberately NOT set: package payloads legitimately
     * contain symlinks whose targets are relative paths inside the same
     * system (e.g. usr/lib/getconf/POSIX_V6_LP64_OFF64 -> usr/bin/getconf),
     * which that flag rejects as "escaping". The payloads are sha256-verified
     * against the repository database before they are ever unpacked. */
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_SECURE_NODOTDOT;
    if (geteuid() == 0) flags |= ARCHIVE_EXTRACT_OWNER;
    archive_write_disk_set_options(ext, flags);

    if (archive_read_open_filename(a, path, 10240) != ARCHIVE_OK) {
        fail("cannot open %s: %s", path, archive_error_string(a));
        archive_read_free(a); archive_write_free(ext);
        return -1;
    }

    /* Extract from inside the target root.
     *
     * CRITICAL: archive_write_disk resolves every relative pathname — and
     * every symlink/hardlink target — against the current working directory.
     * Writing "usr/bin/zstd" without this would scatter the package into
     * whatever directory planet was run from, and link entries such as
     * usr/lib/getconf/POSIX_V6_LP64_OFF64 -> usr/bin/getconf would fail
     * outright. chdir into the root, extract, then always come back. */
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) {
        fail("cannot read current directory: %s", strerror(errno));
        archive_read_free(a); archive_write_free(ext);
        return -1;
    }
    if (chdir(root) != 0) {
        fail("cannot enter root %s: %s", root, strerror(errno));
        archive_read_free(a); archive_write_free(ext);
        return -1;
    }

    int rc = extract_loop(a, ext, path, &files, &nfiles, &cap);

    /* Always restore the working directory, whatever happened above. */
    if (chdir(cwd) != 0)
        fail("warning: could not return to %s: %s", cwd, strerror(errno));

    archive_read_close(a); archive_write_close(ext);
    archive_read_free(a); archive_write_free(ext);

    if (rc != 0) return -1;
    if (files_out) { *files_out = files; *nfiles_out = nfiles; }
    else pkgfile_free_list(files, nfiles);
    return 0;
}int pkgfile_extract_install_script(const char *pkg_path, const char *out_path) {
    struct archive *a = archive_read_new();
    if (!a) { fail("archive_read_new failed"); return -1; }
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, pkg_path, 10240) != ARCHIVE_OK) {
        fail("cannot open %s: %s", pkg_path, archive_error_string(a));
        archive_read_free(a);
        return -1;
    }

    int found = 0, rc = ARCHIVE_OK;
    struct archive_entry *e;
    while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(e);
        if (!name || strcmp(entry_name(name), ".INSTALL") != 0) continue;

        FILE *out = fopen(out_path, "wb");
        if (!out) {
            fail("cannot write %s: %s", out_path, strerror(errno));
            archive_read_free(a);
            return -1;
        }
        const void *blk; size_t sz; la_int64_t off;
        while (archive_read_data_block(a, &blk, &sz, &off) == ARCHIVE_OK)
            if (fwrite(blk, 1, sz, out) != sz) {
                fail("write %s: %s", out_path, strerror(errno));
                fclose(out);
                archive_read_free(a);
                return -1;
            }
        fclose(out);
        chmod(out_path, 0700);      /* the script is sourced by bash, not exec'd */
        found = 1;
        break;
    }
    archive_read_free(a);
    return found;
}

void pkgfile_free_list(char **files, int n) {
    if (!files) return;
    for (int i = 0; i < n; i++) free(files[i]);
    free(files);
}
