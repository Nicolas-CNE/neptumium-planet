/*
 * main.c — the `planet` command-line interface.
 * =============================================
 *
 * Command-line front end for Planet, Neptumium's package manager. Planet
 * resolves dependencies with a from-scratch CDCL MaxSAT solver, so every
 * transaction it reports is provably optimal rather than merely feasible.
 * This file parses the command line, loads the repository databases and
 * presents the solver's results.
 *
 * DESIGN GOALS FOR THE CLI
 *   1. Simple to understand. Every command does one thing; the output is
 *      short, aligned, and colour-coded when stdout is a terminal (plain
 *      text otherwise, honouring NO_COLOR).
 *   2. Honest. When Planet cannot do something (a package does not exist, the
 *      requests are mutually impossible) it says exactly that and exits
 *      non-zero.
 *   3. Debuggable. `-S/--solver-stats` prints the solver's internals (BCP
 *      rate, learned clause DB, VSIDS decay, timing) and `dump-cnf` exports
 *      the exact WCNF instance, so an unexpected plan can be inspected with
 *      any external MaxSAT tool.
 *
 * COMMANDS
 *   sync      fetch the repository databases into the local cache
 *   install   download, verify, extract and record packages
 *   remove    remove packages and the files they own
 *   upgrade   move installed packages to the newest feasible set
 *   autoremove remove packages nothing else requires
 *   lock      write the installed system state to a lock file
 *   search    find packages by name or description
 *   info      show one package in full
 *   stats     summarise the loaded repository
 *   solve     compute the optimal package set for a request list
 *   dump-cnf  export the MaxSAT instance for a request list (WCNF)
 *   doctor    health check: cache, databases, network, target root
 *   help      this text;  version: print the version
 *
 * TRANSACTIONS
 *   install/remove/upgrade/autoremove share one path: build a request list
 *   (everything installed that must stay as HARD requirements, plus the new
 *   wishes), let the solver find the optimal concrete set, then apply the
 *   difference. Because the target state is computed rather than guessed, a
 *   transaction can never orphan a dependency or install a conflicting
 *   package.
 *
 * REQUEST SYNTAX: arguments accept the same constraint syntax as the repos
 * themselves, so `planet install 'vim>=9' 'gcc=15'` pins versions the same way
 * a depends line would.
 */
#define _GNU_SOURCE  /* strcasestr() */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <archive.h>
#include <curl/curl.h>

#include "archrepo.h"
#include "encode.h"
#include "hooks.h"
#include "localdb.h"
#include "net.h"
#include "pkg.h"
#include "pkgfile.h"

#define PLANET_VERSION "0.1.0"

/* ---- colour handling ----------------------------------------------------- */

/* Colour is optional: it is disabled when stdout is not a terminal or when
 * NO_COLOR is set (the de-facto standard for disabling ANSI output). */
static int g_color = 0;
static int g_yes = 0;   /* -y/--yes: skip confirmation prompts */

#define C_BOLD   (g_color ? "\033[1m"  : "")
#define C_DIM    (g_color ? "\033[2m"  : "")
#define C_CYAN   (g_color ? "\033[36m" : "")
#define C_GREEN  (g_color ? "\033[32m" : "")
#define C_YELLOW (g_color ? "\033[33m" : "")
#define C_RED    (g_color ? "\033[31m" : "")
#define C_OFF    (g_color ? "\033[0m"  : "")

/* ---- tiny utilities ------------------------------------------------------ */

/* Fatal user-facing error: prints "planet: <msg>" and exits with `code`. */
static void die(int code, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

static void die(int code, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%splanet:%s ", C_RED, C_OFF);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(code);
}

/* Monotonic milliseconds, for --solver-stats timing. */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Human-readable byte count: "912 B", "3.2 MiB", "1.4 GiB". */
static const char *hr_size(long bytes) {
    static char buf[32];
    if (bytes < 0) { snprintf(buf, sizeof buf, "?"); return buf; }
    if (bytes < 1024) { snprintf(buf, sizeof buf, "%ld B", bytes); return buf; }
    double v = (double)bytes;
    int    u = -1;                     /* -1 = still in bytes */
    while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
    static const char *unit[] = {"KiB", "MiB", "GiB", "TiB"};
    snprintf(buf, sizeof buf, "%.1f %s", v, unit[u]);
    return buf;
}

/* ---- global options ------------------------------------------------------ */

typedef struct {
    const char *root;       /* target system root, default "/"              */
    const char *cache_dir;  /* where the repo databases live                */
    const char *out_file;   /* -o target for dump-cnf                       */
    int   dry_run;
    int   verbose;
    int   solver_stats;
    int   yes;              /* -y/--yes: skip confirmation prompts          */
    int   demo;             /* use the built-in demo repository             */
} options;

/* A configured repository: a name and the mirror directory that serves its
 * database. Defaults are the official Arch geo-mirrors; --repo overrides. */
typedef struct { char *name; char *url; } repo_src;

#define MAX_REPOS 8
static repo_src g_repos[MAX_REPOS];
static int      g_nrepos = 0;

static void repo_src_add(const char *name, const char *url) {
    if (g_nrepos == MAX_REPOS) die(2, "too many repositories (max %d)", MAX_REPOS);
    g_repos[g_nrepos].name = strdup(name);
    g_repos[g_nrepos].url  = strdup(url);
    if (!g_repos[g_nrepos].name || !g_repos[g_nrepos].url)
        die(1, "out of memory");
    g_nrepos++;
}

static void repos_default(void) {
    repo_src_add("core",  "https://geo.mirror.pkgbuild.com/core/os/x86_64/");
    repo_src_add("extra", "https://geo.mirror.pkgbuild.com/extra/os/x86_64/");
}

/* The cache path for a repo database: <cache>/<name>.db.tar.gz. */
static char *db_cache_path(const char *cache_dir, const char *name) {
    size_t n = strlen(cache_dir) + strlen(name) + 16;
    char  *p = malloc(n);
    if (!p) die(1, "out of memory");
    snprintf(p, n, "%s/%s.db.tar.gz", cache_dir, name);
    return p;
}

/* ---- the demo repository ------------------------------------------------- */

/* Populate `r` with a hand-built mini repository so `planet solve --demo`
 * works with no network and no cache. It exercises every encoding rule at
 * once:
 *   - two versions of bash        -> version preference must pick the newest;
 *   - libfoo.so provided by foo   -> a virtual dependency resolves to a provider;
 *   - editor/oldeditor rename     -> Conflicts + Replaces on the same name
 *                                    (the Arch rename idiom);
 *   - an epoch on heavyapp        -> epoch ordering must survive;
 *   - a deliberate conflict pair  -> the solver must refuse to combine them. */
static void build_demo_repo(repo *r) {
    struct { const char *name, *ver, *desc; } simple[] = {
        {"bash",   "5.2.037-1", "The GNU Bourne Again shell (old)"},
        {"bash",   "5.3.20-1",  "The GNU Bourne Again shell"},
        {"glibc",  "2.40-3",    "GNU C Library"},
        {"readline", "8.2-1",   "GNU readline library"},
        {"ncurses", "6.5-2",    "System V Release 4.0 curses"},
        {"git",    "2.51.0-1",  "The fast distributed version control system"},
        {"pcre2",  "10.44-1",   "A library implementing Perl-compatible regex"},
        {"curl",   "8.18.0-1",  "command line tool for transferring data"},
        {"openssl", "3.5.9-1",  "The Open Source toolkit for SSL/TLS"},
        {"heavyapp", "1:3.0-1", "Demo app with an epoch"},
    };
    for (size_t i = 0; i < sizeof simple / sizeof simple[0]; i++) {
        package *p = pkg_new(simple[i].name, simple[i].ver, "x86_64");
        if (!p) die(1, "out of memory");
        pkg_set_desc(p, simple[i].desc);
        repo_add(r, p);
    }

    /* bash 5.3 needs what a real bash needs (versioned soname dep included) */
    {
        const package *newest_bash = repo_newest(r, "bash", DEP_ANY, NULL);
        package *p = (package *)newest_bash;
        pkg_add_dep(p, "readline>=8");
        pkg_add_dep(p, "ncurses");
        pkg_add_dep(p, "glibc");
        pkg_add_provide(p, "sh");
        pkg_add_provide(p, "libreadline.so=8-64");
    }
    {
        package *p = (package *)repo_newest(r, "readline", DEP_ANY, NULL);
        pkg_add_provide(p, "libreadline.so=8-64");
        pkg_add_dep(p, "glibc");
    }
    {   /* curl pulls a chain deep enough to be interesting */
        package *p = (package *)repo_newest(r, "curl", DEP_ANY, NULL);
        pkg_add_dep(p, "openssl>=3");
        pkg_add_dep(p, "pcre2>=10.30");
        pkg_add_dep(p, "glibc");
    }
    {   /* git: the deepest chain, plus the virtual 'sh' */
        package *p = (package *)repo_newest(r, "git", DEP_ANY, NULL);
        pkg_add_dep(p, "curl");
        pkg_add_dep(p, "pcre2");
        pkg_add_dep(p, "openssl");
        pkg_add_dep(p, "glibc");
    }
    {   /* heavyapp: epoch + a virtual dep on libfoo.so */
        package *p = (package *)repo_newest(r, "heavyapp", DEP_ANY, NULL);
        pkg_add_dep(p, "libfoo.so>=2");
    }
    {   /* foo provides the virtual libfoo.so, so heavyapp is installable */
        package *p = pkg_new("foo", "2.1-1", "x86_64");
        pkg_set_desc(p, "Provider of libfoo.so (demo)");
        pkg_add_provide(p, "libfoo.so=2.5");
        pkg_add_dep(p, "glibc");
        repo_add(r, p);
    }
    {   /* the rename idiom: new package conflicts AND replaces the old one,
         * while also providing it so dependents keep working */
        package *p = pkg_new("editor", "2.0-1", "x86_64");
        pkg_set_desc(p, "The new editor (renamed from oldeditor)");
        pkg_add_conflict(p, "oldeditor");
        pkg_add_replace(p, "oldeditor");
        pkg_add_provide(p, "oldeditor");
        pkg_add_dep(p, "glibc");
        repo_add(r, p);
        package *o = pkg_new("oldeditor", "1.0-1", "x86_64");
        pkg_set_desc(o, "The old editor (superseded)");
        pkg_add_dep(o, "glibc");
        repo_add(r, o);
    }
}

/* ---- repository loading -------------------------------------------------- */

/* Load every configured repo database from the cache into one merged repo.
 * Databases that are missing are reported (with a sync hint) and skipped
 * unless none at all could be loaded, which is fatal. */
static repo *load_repo(const options *o) {
    repo *r = repo_new();
    if (!r) die(1, "out of memory");

    if (o->demo) {
        build_demo_repo(r);
        return r;
    }

    int loaded = 0;
    for (int i = 0; i < g_nrepos; i++) {
        char *path = db_cache_path(o->cache_dir, g_repos[i].name);
        if (access(path, R_OK) != 0) {
            fprintf(stderr, "%swarning:%s no cached database for '%s' (%s) "
                            "— run 'planet sync'\n",
                    C_YELLOW, C_OFF, g_repos[i].name, path);
            free(path);
            continue;
        }
        archrepo_stats st;
        char err[256] = {0};
        if (archrepo_load_file(r, path, g_repos[i].url, &st, err, sizeof err)
            != 0) {
            fprintf(stderr, "%serror:%s cannot parse %s: %s\n",
                    C_RED, C_OFF, path, err);
            free(path);
            continue;
        }
        if (o->verbose)
            printf("  %s: %d packages (%d duplicates skipped, %d malformed)\n",
                   g_repos[i].name, st.packages, st.duplicates, st.malformed);
        free(path);
        loaded++;
    }

    if (loaded == 0) {
        repo_free(r);
        die(1, "no repository databases available — run 'planet sync' first "
                "(or use --demo for the built-in example repo)");
    }
    return r;
}

/* ---- transaction planning ------------------------------------------------- */

/* Load the installed packages from the local database into `r` and mark them
 * as installed candidates. The solver then sees the full system state: the
 * remote repos offer versions, and the local db pins what is already on disk.
 * Returns the number of installed packages loaded, or -1 on failure. */
static int load_system_state(repo *r, const char *root) {
    int n = 0;
    package **inst = localdb_list(root, &n);
    if (!inst) return -1;
    for (int i = 0; i < n; i++) {
        /* If the repository already offers this exact name+version, mark that
         * candidate installed instead of adding a duplicate. Two candidates
         * for one name+version would share an AMO group, so the solver could
         * pick the repo copy and "remove" the installed one — showing a bogus
         * reinstall of a package that is already on disk. */
        const package *existing = repo_find(r, inst[i]->name, inst[i]->version);
        if (existing) {
            ((package *)existing)->installed = 1;
            pkg_free(inst[i]);   /* redundant copy: the repo owns `existing` */
            continue;
        }
        inst[i]->installed = 1;
        /* An installed package that the repos no longer offer: no download
         * origin, so it is treated as already on disk. */
        free(inst[i]->origin);
        inst[i]->origin = NULL;
        if (!repo_add(r, inst[i])) {
            localdb_free_list(inst);
            return -1;
        }
    }
    /* Added packages are now owned by r; free the array but not the packages. */
    free(inst);
    return n;
}

/* Build the request list for a transaction: `add` packages (install) minus
 * `drop` packages (remove), while keeping every currently-installed package
 * that is not being removed. This is the uniform model for install/remove/
 * upgrade — the solver picks the optimal concrete set satisfying it. */
typedef struct {
    dep *reqs;      /* soft wishes (install)                          */
    int  n;
    dep *keep;      /* hard requirements (must stay installed)         */
    int  nkeep;
} request_list;

/* Build the request lists for a transaction: `add` packages (soft install
 * wishes) minus `drop` packages (hard removals), while pinning every
 * currently-installed package that is not being removed as a HARD keep. */
static request_list build_transaction(const char *root,
                                      const dep *add, int nadd,
                                      const dep *drop, int ndrop) {
    int ncap = 64, kcap = 64;
    request_list rl = { malloc((size_t)ncap * sizeof *rl.reqs), 0,
                        malloc((size_t)kcap * sizeof *rl.keep), 0 };
    if (!rl.reqs || !rl.keep) {
        free(rl.reqs); free(rl.keep);
        request_list empty = { NULL, 0, NULL, 0 };
        return empty;
    }

    /* Hard keeps: every installed package stays, unless it is being removed. */
    int ninst = 0;
    package **inst = localdb_list(root, &ninst);
    for (int i = 0; inst && i < ninst; i++) {
        int dropping = 0;
        for (int k = 0; k < ndrop; k++)
            if (strcmp(inst[i]->name, drop[k].name) == 0) { dropping = 1; break; }
        if (dropping) continue;
        if (rl.nkeep == kcap) {
            kcap *= 2;
            dep *nk = realloc(rl.keep, (size_t)kcap * sizeof *nk);
            if (!nk) { free(rl.reqs); free(rl.keep); request_list e2 = { NULL, 0, NULL, 0 }; return e2; }
            rl.keep = nk;
        }
        char *spec = malloc(strlen(inst[i]->name) + strlen(inst[i]->version) + 2);
        if (spec) {
            sprintf(spec, "%s=%s", inst[i]->name, inst[i]->version);
            dep d;
            if (dep_parse(&d, spec)) rl.keep[rl.nkeep++] = d;
            free(spec);
        }
    }
    localdb_free_list(inst);

    /* Soft adds: the explicitly requested packages (already parsed deps). */
    for (int i = 0; i < nadd; i++) {
        if (rl.n == ncap) {
            ncap *= 2;
            dep *nr = realloc(rl.reqs, (size_t)ncap * sizeof *nr);
            if (!nr) { free(rl.reqs); free(rl.keep); request_list e2 = { NULL, 0, NULL, 0 }; return e2; }
            rl.reqs = nr;
        }
        rl.reqs[rl.n] = (dep){ strdup(add[i].name), add[i].op,
                               add[i].version ? strdup(add[i].version) : NULL };
        rl.n++;
    }
    return rl;
}

static void request_list_free(request_list *rl) {
    for (int i = 0; i < rl->n; i++)     dep_free(&rl->reqs[i]);
    for (int i = 0; i < rl->nkeep; i++) dep_free(&rl->keep[i]);
    free(rl->reqs);
    free(rl->keep);
}

/* Parse request arguments ("vim", "gcc>=12", ...) into deps. Returns the
 * count; dies with a clear message on malformed input. */
static dep *parse_requests(int argc, char **argv, int *n_out) {
    dep *d = calloc((size_t)argc, sizeof *d);
    if (!d) die(1, "out of memory");
    for (int i = 0; i < argc; i++) {
        if (!dep_parse(&d[i], argv[i]))
            die(2, "cannot understand request '%s' (try 'name', 'name>=1.2', "
                   "'name=1.2-1')", argv[i]);
    }
    *n_out = argc;
    return d;
}

/* ---- transaction commands ------------------------------------------------- */

static int ask_yes(const char *prompt) {
    if (g_yes) return 1;
    printf("%s [y/N] ", prompt);
    fflush(stdout);
    char buf[16] = {0};
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

/* Print the transaction plan and, unless --dry-run/--yes, ask for
 * confirmation. Returns 1 to proceed, 0 to abort. `dry_run` makes the caller
 * print the plan and stop without touching anything. */
static int confirm_transaction(const char *label, int dry_run,
                               const package **install, int ninstall,
                               const package **remove, int nremove) {
    printf("%sTransaction plan%s\n", C_BOLD, C_OFF);
    for (int i = 0; i < ninstall; i++)
        printf("  %s+%s %-24s %s\n", C_GREEN, C_OFF,
               install[i]->name, install[i]->version);
    for (int i = 0; i < nremove; i++)
        printf("  %s-%s %-24s %s\n", C_RED, C_OFF,
               remove[i]->name, remove[i]->version);
    if (ninstall == 0 && nremove == 0) {
        printf("  %s(nothing to do)%s\n", C_DIM, C_OFF);
        return 1;
    }
    if (dry_run) return 0;   /* plan shown; nothing will be applied */
    return ask_yes("Apply this transaction?");
}

/* Install one package: fetch+verify (cached), extract, record in local db.
 * Packages without download metadata (origin/filename) are treated as
 * already present (demo/offline): skip fetch/extract but still record them. */
static int do_install_one(const options *o, const package *p) {
    char **files = NULL;
    int    nfiles = 0;

    if (p->origin && p->filename) {
        char *path = pkgfile_fetch(p);
        if (!path) {
            fprintf(stderr, "%serror:%s %s: %s\n", C_RED, C_OFF, p->name,
                    pkgfile_error());
            return -1;
        }
        if (pkgfile_extract(path, o->root, &files, &nfiles) != 0) {
            fprintf(stderr, "%serror:%s extract %s: %s\n", C_RED, C_OFF, p->name,
                    pkgfile_error());
            free(path);
            return -1;
        }
        free(path);
    }

    /* Record the package together with the files it owns, so removal can
     * delete exactly those paths. */
    if (localdb_add(o->root, p, (const char *const *)files, nfiles) != 0) {
        fprintf(stderr, "%serror:%s could not record %s in local db: %s\n",
                C_YELLOW, C_OFF, p->name, localdb_error());
        pkgfile_free_list(files, nfiles);
        return -1;
    }
    if (o->verbose)
        printf("    %srecorded %d file(s) for %s%s\n", C_DIM, nfiles, p->name, C_OFF);
    pkgfile_free_list(files, nfiles);
    return 0;
}

/* Remove one package: delete the files it owns (from the local db file list),
 * deepest-first so directories are empty before they are removed, then drop
 * the local db entry. Paths already gone are ignored (idempotent removal). */
static int do_remove_one(const options *o, const package *p) {
    char *base = localdb_path(o->root);
    if (!base) return -1;
    size_t n = strlen(base) + strlen(p->name) + strlen(p->version) + 16;
    char *files = malloc(n);
    snprintf(files, n, "%s/%s-%s/files", base, p->name, p->version);

    int removed = 0, failed = 0;
    FILE *f = fopen(files, "r");
    if (f) {
        char line[4096];
        /* Collect first, then delete in reverse: the archive stores
         * directories before their contents, so reverse order empties them. */
        char **paths = NULL;
        int np = 0, cap = 0;
        while (fgets(line, sizeof line, f)) {
            size_t len = strlen(line);
            while (len && (line[len-1] == '\n' || line[len-1] == '\r'))
                line[--len] = '\0';
            if (len == 0) continue;
            if (np == cap) {
                cap = cap ? cap * 2 : 64;
                char **npaths = realloc(paths, (size_t)cap * sizeof *npaths);
                if (!npaths) { failed = 1; break; }
                paths = npaths;
            }
            paths[np] = strdup(line);
            if (!paths[np]) { failed = 1; break; }
            np++;
        }
        fclose(f);

        for (int i = np - 1; i >= 0; i--) {
            size_t fl = strlen(o->root) + strlen(paths[i]) + 2;
            char *full = malloc(fl);
            if (!full) { failed = 1; break; }
            snprintf(full, fl, "%s/%s", o->root, paths[i]);
            struct stat st;
            if (lstat(full, &st) == 0) {
                /* Directories need rmdir; everything else unlink handles
                 * (files, symlinks). A non-empty directory means another
                 * package still owns something inside — leave it alone. */
                int rc = S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
                if (rc != 0 && errno != ENOENT) {
                    if (o->verbose)
                        fprintf(stderr, "%swarning:%s could not delete %s%s\n",
                                C_YELLOW, C_OFF, paths[i], C_OFF);
                } else {
                    removed++;
                }
            }
            free(full);
        }
        for (int i = 0; i < np; i++) free(paths[i]);
        free(paths);
    }
    free(files);
    free(base);

    if (localdb_remove(o->root, p->name, p->version) != 0) {
        fprintf(stderr, "%serror:%s %s\n", C_RED, C_OFF, localdb_error());
        return -1;
    }
    if (o->verbose)
        printf("    %sdeleted %d path(s)%s\n", C_DIM, removed, C_OFF);
    return failed ? -1 : 0;
}

/* Shared transaction engine: load repos + system state, build the request
 * list, solve, diff old vs new, confirm and execute. Returns 0 on success. */
    /* Which of `pkgs` share a NAME with something currently installed? Those are
 * upgrades (and so run the *_upgrade hooks); the rest are fresh installs.
 * `is_upgrade` must have room for npkgs entries. */
static void classify_upgrades(const package **pkgs, int npkgs,
                              const options *o, unsigned char *is_upgrade) {
    int ninst = 0;
    package **inst = localdb_list(o->root, &ninst);
    for (int i = 0; i < npkgs; i++) {
        is_upgrade[i] = 0;
        for (int k = 0; inst && k < ninst; k++)
            if (strcmp(pkgs[i]->name, inst[k]->name) == 0) { is_upgrade[i] = 1; break; }
    }
    localdb_free_list(inst);
}

/* Run a package's .INSTALL script for `phase`, reporting failures. Returns 0
 * if the hook is absent or succeeded, -1 if it failed. */
static int run_pkg_hook(const options *o, const package *p, install_phase phase) {
    if (!hooks_enabled()) return 0;
    if (hooks_run_install_script(p, o->root, phase, NULL) == 0) return 0;
    fprintf(stderr, "%shook error:%s %s: %s\n", C_RED, C_OFF,
            install_phase_func(phase), hooks_error());
    return -1;
}

static int run_transaction(const options *o, const char *label,
                           const dep *add, int nadd,
                           const dep *drop, int ndrop) {
    /* Keep the payload cache beside the database cache the user chose. */
    pkgfile_cache_dir_set(o->cache_dir);
    repo *r = load_repo(o);
    if (!r) die(1, "cannot load repositories");
    load_system_state(r, o->root);

    request_list rl = build_transaction(o->root, add, nadd, drop, ndrop);
    if (!rl.reqs) die(1, "out of memory");

    encoder *e = enc_new(r);
    if (!e) { request_list_free(&rl); repo_free(r); die(1, "out of memory"); }
    /* Hard keeps first: packages that must stay installed. */
    for (int i = 0; i < rl.nkeep; i++)
        if (!enc_require(e, rl.keep[i].name, rl.keep[i].op, rl.keep[i].version))
            die(1, "cannot keep '%s': nothing in the repositories provides it",
                rl.keep[i].name);
    /* Soft adds: install wishes the solver may break only at a cost. */
    for (int i = 0; i < rl.n; i++)
        if (!enc_request(e, rl.reqs[i].name, rl.reqs[i].op, rl.reqs[i].version, 0))
            die(1, "nothing provides '%s'", rl.reqs[i].name);

    double t0 = now_ms();
    int rc = enc_solve(e);
    double dt = now_ms() - t0;
    if (rc != MAXSAT_OPTIMAL) {
        fprintf(stderr, "%serror:%s cannot satisfy that request\n", C_RED, C_OFF);
        request_list_free(&rl); enc_free(e); repo_free(r);
        return 1;
    }

    /* Diff: chosen-and-not-installed = to install; installed-and-not-chosen =
     * to remove. */
    int nci = 0, ncr = 0;
    const package **to_install = malloc((size_t)(enc_ncandidates(e) ? enc_ncandidates(e) : 1) * sizeof *to_install);
    const package **to_remove  = malloc((size_t)(enc_ncandidates(e) ? enc_ncandidates(e) : 1) * sizeof *to_remove);
    for (int i = 0; i < enc_ncandidates(e); i++) {
        const package *p = enc_candidate(e, i);
        int chosen = enc_chosen(e, i);
        if (chosen && !p->installed) to_install[nci++] = p;
        if (!chosen && p->installed)  to_remove[ncr++] = p;
    }

    if (!confirm_transaction(label, o->dry_run, to_install, nci, to_remove, ncr)) {
        if (o->dry_run)
            printf("%sdry run: nothing was changed%s\n", C_DIM, C_OFF);
        else
            fprintf(stderr, "aborted\n");
        free(to_install); free(to_remove);
        request_list_free(&rl); enc_free(e); repo_free(r);
        return 0;
    }


    int errs = 0;
    unsigned char *upg = calloc((size_t)(nci ? nci : 1), 1);
    if (!upg) { free(to_install); free(to_remove); request_list_free(&rl); enc_free(e); repo_free(r); die(1, "out of memory"); }
    if (nci) classify_upgrades(to_install, nci, o, upg);

    /* Repository hooks around the whole transaction. */
    if (nci) hooks_run_repo(o->root, "Install", HP_PRE_TRANSACTION,
                            to_install, nci);
    if (ncr) hooks_run_repo(o->root, "Remove", HP_PRE_TRANSACTION,
                            to_remove, ncr);

    for (int i = 0; i < nci; i++) {
        printf("  %sinstalling%s %s %s\n", C_GREEN, C_OFF,
               to_install[i]->name, to_install[i]->version);
        /* pre_install for a new package, pre_upgrade when replacing one. */
        run_pkg_hook(o, to_install[i],
                     upg[i] ? IP_PRE_UPGRADE : IP_PRE_INSTALL);
        hooks_run_repo(o->root, "Install",
                       upg[i] ? HP_PRE_UPGRADE : HP_PRE_INSTALL,
                       &to_install[i], 1);
        if (do_install_one(o, to_install[i]) != 0) { errs++; continue; }
        run_pkg_hook(o, to_install[i],
                     upg[i] ? IP_POST_UPGRADE : IP_POST_INSTALL);
        hooks_run_repo(o->root, "Install",
                       upg[i] ? HP_POST_UPGRADE : HP_POST_INSTALL,
                       &to_install[i], 1);
    }
    for (int i = 0; i < ncr; i++) {
        printf("  %sremoving%s  %s %s\n", C_RED, C_OFF,
               to_remove[i]->name, to_remove[i]->version);
        run_pkg_hook(o, to_remove[i], IP_PRE_REMOVE);
        hooks_run_repo(o->root, "Remove", HP_PRE_REMOVE, &to_remove[i], 1);
        if (do_remove_one(o, to_remove[i]) != 0) { errs++; continue; }
        run_pkg_hook(o, to_remove[i], IP_POST_REMOVE);
        hooks_run_repo(o->root, "Remove", HP_POST_REMOVE, &to_remove[i], 1);
    }
    free(upg);

    if (nci) hooks_run_repo(o->root, "Install", HP_POST_TRANSACTION,
                            to_install, nci);
    if (ncr) hooks_run_repo(o->root, "Remove", HP_POST_TRANSACTION,
                            to_remove, ncr);

    if (o->solver_stats) {
        sat_stats st;
        enc_stats(e, &st);
        printf("\n  %sSolver%s: %ld conflicts, %ld propagations (%.1f/decision), "
               "%ld learned, VSIDS %.3f, %.2f ms\n", C_BOLD, C_OFF,
               st.conflicts, st.propagations,
               st.decisions ? (double)st.propagations / st.decisions : 0.0,
               st.learnts, (double)st.vsids_decay, dt);
    }

    if (errs)
        fprintf(stderr, "%s%d package(s) failed to apply%s\n", C_RED, errs, C_OFF);

    free(to_install); free(to_remove);
    request_list_free(&rl); enc_free(e); repo_free(r);
    return errs ? 1 : 0;
}

static int cmd_install(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "install needs at least one package name");
    dep *reqs = parse_requests(argc, argv, &argc);
    int rc = run_transaction(o, "Install", reqs, argc, NULL, 0);
    for (int i = 0; i < argc; i++) dep_free(&reqs[i]);
    free(reqs);
    return rc;
}

static int cmd_remove(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "remove needs at least one package name");
    dep *reqs = parse_requests(argc, argv, &argc);
    int rc = run_transaction(o, "Remove", NULL, 0, reqs, argc);
    for (int i = 0; i < argc; i++) dep_free(&reqs[i]);
    free(reqs);
    return rc;
}

static int cmd_upgrade(const options *o, int argc, char **argv) {
    /* Upgrade = keep everything installed (at >= current version), resolve to
     * the newest feasible set, apply the delta. Extra args pin versions. */
    dep *pins = parse_requests(argc, argv, &argc);
    int rc = run_transaction(o, "Upgrade", pins, argc, NULL, 0);
    for (int i = 0; i < argc; i++) dep_free(&pins[i]);
    free(pins);
    return rc;
}

static int cmd_autoremove(const options *o, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Orphans: installed packages that nothing else requires. Solve for the
     * system with all non-orphan installed packages kept, and remove the rest. */
    repo *r = load_repo(o);
    if (!r) die(1, "cannot load repositories");
    load_system_state(r, o->root);

    int ninst = 0;
    package **inst = localdb_list(o->root, &ninst);
    if (!inst) die(1, "cannot read local db");

    /* Build a request list keeping every installed package that is required by
     * at least one other installed package (directly or via provides). */
    int ncap = 64, nkeep = 0;
    dep *keep = malloc((size_t)ncap * sizeof *keep);
    for (int i = 0; i < ninst; i++) {
        int required = 0;
        for (int j = 0; j < ninst; j++) {
            if (i == j) continue;
            for (int d = 0; d < inst[j]->ndepends; d++)
                if (pkg_provides(inst[i], inst[j]->depends[d].name,
                                  inst[j]->depends[d].op, inst[j]->depends[d].version)) {
                    required = 1; break;
                }
            if (required) break;
        }
        if (!required) continue;   /* orphan candidate: don't keep it */
        if (nkeep == ncap) { ncap *= 2; keep = realloc(keep, (size_t)ncap * sizeof *keep); }
        char *spec = malloc(strlen(inst[i]->name) + strlen(inst[i]->version) + 2);
        sprintf(spec, "%s=%s", inst[i]->name, inst[i]->version);
        dep d;
        if (dep_parse(&d, spec)) keep[nkeep++] = d;
        free(spec);
    }

    encoder *e = enc_new(r);
    if (!e) { free(keep); localdb_free_list(inst); repo_free(r); die(1, "out of memory"); }
    for (int i = 0; i < nkeep; i++)
        if (!enc_require(e, keep[i].name, keep[i].op, keep[i].version))
            die(1, "internal: request '%s' not satisfiable", keep[i].name);
    int rc = enc_solve(e);
    if (rc != MAXSAT_OPTIMAL) {
        fprintf(stderr, "%serror:%s cannot resolve the system\n", C_RED, C_OFF);
        for (int i = 0; i < nkeep; i++) dep_free(&keep[i]);
        free(keep); localdb_free_list(inst); enc_free(e); repo_free(r);
        return 1;
    }

    /* Remove installed packages the solver did not keep. */
    int errs = 0, nremoved = 0;
    for (int i = 0; i < ninst; i++) {
        int still = 0;
        for (int k = 0; k < enc_ncandidates(e); k++)
            if (enc_chosen(e, k) && enc_candidate(e, k) == inst[i]) { still = 1; break; }
        if (still) continue;
        printf("  %sremoving%s  %s %s (orphan)\n", C_RED, C_OFF,
               inst[i]->name, inst[i]->version);
        if (do_remove_one(o, inst[i]) != 0) errs++;
        nremoved++;
    }
    if (nremoved == 0) printf("  %sno orphans found%s\n", C_DIM, C_OFF);

    for (int i = 0; i < nkeep; i++) dep_free(&keep[i]);
    free(keep); localdb_free_list(inst); enc_free(e); repo_free(r);
    return errs ? 1 : 0;
}

static int cmd_lock(const options *o, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Lock the current system: write /var/lib/planet/installed.lock with the
     * exact set of installed name=version pairs. Applying the lock later
     * reproduces this system state exactly. */
    int n = 0;
    package **inst = localdb_list(o->root, &n);
    if (!inst) die(1, "cannot read local db");

    char *path = malloc(strlen(o->root) + 64);
    snprintf(path, strlen(o->root) + 64, "%s/var/lib/planet/installed.lock", o->root);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "%serror:%s cannot write %s: %s\n", C_RED, C_OFF, path,
                strerror(errno));
        free(path); localdb_free_list(inst);
        return 1;
    }
    fprintf(f, "# planet lock file — generated %s", ctime(&(time_t){time(NULL)}));
    for (int i = 0; i < n; i++)
        fprintf(f, "%s=%s\n", inst[i]->name, inst[i]->version);
    fclose(f);
    printf("%swrote %d package(s) to %s%s\n", C_GREEN, n, path, C_OFF);
    free(path); localdb_free_list(inst);
    return 0;
}

/* ---- command: sync -------------------------------------------------------- */

/* Download every configured database into the cache and parse it, printing a
 * one-line summary per repo. --dry-run fetches and reports without writing. */
static int cmd_sync(const options *o) {
    if (net_global_init() != 0) die(1, "network initialisation failed");

    if (o->dry_run)
        printf("Synchronising repositories %s%s(dry run)%s\n",
               " ", C_DIM, C_OFF);
    else
        printf("Synchronising repositories\n");

    for (int i = 0; i < g_nrepos; i++) {
        const char *name = g_repos[i].name;
        size_t ulen = strlen(g_repos[i].url) + strlen(name) + 16;
        char *url = malloc(ulen);
        if (!url) die(1, "out of memory");
        /* Databases live at <mirror-dir>/<name>.db.tar.gz */
        snprintf(url, ulen, "%s%s.db.tar.gz", g_repos[i].url, name);

        char *path = NULL;
        FILE *fp = NULL;
        if (!o->dry_run) {
            path = db_cache_path(o->cache_dir, name);
            fp = fopen(path, "wb");
            if (!fp) die(1, "cannot write cache file %s (does the cache "
                            "directory exist? try 'mkdir -p %s')",
                            path, o->cache_dir);
        }

        double t0 = now_ms();
        if (o->verbose) printf("  fetching %s\n", url);

        int rc;
        if (fp) {
            /* Stream straight to the cache file, then parse from disk — the
             * file is the offline artefact we wanted anyway. */
            rc = net_fetch_to_file(url, fp, 0);
            fclose(fp);
        } else {
            /* Dry run: fetch to memory, parse, discard. */
            unsigned char *buf = NULL;
            size_t len = 0;
            rc = net_fetch(url, &buf, &len, 0);
            if (rc == 0) {
                archrepo_stats st;
                char err[256] = {0};
                repo *tmp = repo_new();
                rc = archrepo_load_mem(tmp, buf, len, g_repos[i].url, &st,
                                       err, sizeof err);
                if (rc == 0)
                    printf("  %-6s %s%d packages%s (%.0f ms, nothing written)\n",
                           name, C_GREEN, st.packages, C_OFF, now_ms() - t0);
                else
                    printf("  %-6s %sparse error:%s %s\n", name, C_RED, C_OFF,
                           err);
                repo_free(tmp);
                free(buf);
            }
        }

        if (rc != 0) {
            if (path) remove(path);   /* never leave a truncated db behind */
            const char *why = net_error();
            fprintf(stderr, "  %-6s %sfailed:%s %s\n", name, C_RED, C_OFF,
                    why && *why ? why : "network error");
            free(url); free(path);
            net_global_cleanup();
            return 1;
        }

        if (!o->dry_run) {
            /* Parse the freshly written file for the summary line. */
            archrepo_stats st;
            char err[256] = {0};
            repo *tmp = repo_new();
            if (archrepo_load_file(tmp, path, g_repos[i].url, &st, err,
                                   sizeof err) != 0)
                fprintf(stderr, "  %-6s %swritten but unparseable:%s %s\n",
                        name, C_RED, C_OFF, err);
            else
                printf("  %-6s %s%d packages%s  %s\n", name, C_GREEN,
                       st.packages, C_OFF, path);
            repo_free(tmp);
        }

        free(url);
        free(path);
    }

    net_global_cleanup();
    printf("%sDone.%s\n", C_DIM, C_OFF);
    return 0;
}

/* ---- command: search ------------------------------------------------------ */

static int cmd_search(const options *o, const char *term) {
    repo *r = load_repo(o);
    int   shown = 0;

    printf("%s%-24s %-16s %sDescription%s\n", C_BOLD, "Name", "Version", "", C_OFF);
    for (int i = 0; i < repo_count(r); i++) {
        const package *p = repo_get(r, i);
        int hit = strcasestr(p->name, term) != NULL
                  || (p->desc && strcasestr(p->desc, term) != NULL);
        if (!hit) continue;
        printf("%s%-24s%s %-16s %s\n", C_CYAN, p->name, C_OFF, p->version,
               p->desc ? p->desc : "");
        shown++;
    }
    if (shown == 0) printf("%s(no matches for '%s')%s\n", C_DIM, term, C_OFF);
    else            printf("%s%d match(es)%s\n", C_DIM, shown, C_OFF);
    repo_free(r);
    return shown ? 0 : 1;
}

/* ---- command: info -------------------------------------------------------- */

static int cmd_info(const options *o, const char *name) {
    repo *r = load_repo(o);
    const package *p = repo_newest(r, name, DEP_ANY, NULL);
    if (!p) {
        fprintf(stderr, "%serror:%s no package named '%s'\n", C_RED, C_OFF, name);
        repo_free(r);
        return 1;
    }

    printf("%s%s %s%s\n", C_BOLD, p->name, p->version, C_OFF);
    if (p->desc) printf("    %s\n", p->desc);
    printf("    repository url  : %s\n", p->origin ? p->origin : "-");
    printf("    filename        : %s\n", p->filename ? p->filename : "-");
    printf("    download size   : %s\n", p->csize > 0 ? hr_size(p->csize) : "-");
    printf("    installed size  : %s\n", p->isize > 0 ? hr_size(p->isize) : "-");
    printf("    sha256          : %s\n", p->sha256 ? p->sha256 : "-");

    /* How many versions of this name does the repo carry? */
    const package **all = malloc((size_t)(repo_count(r) ? repo_count(r) : 1)
                                 * sizeof *all);
    int n = all ? repo_lookup(r, name, DEP_ANY, NULL, all, repo_count(r)) : 0;
    if (n > 1) {
        printf("    versions        :");
        for (int i = 0; i < n; i++) printf(" %s", all[i]->version);
        printf("\n");
    }
    free(all);

    if (p->ndepends) {
        printf("    %sdepends on%s     :", C_DIM, C_OFF);
        for (int i = 0; i < p->ndepends; i++)
            printf(" %s%s%s", p->depends[i].name, dep_op_str(p->depends[i].op),
                   p->depends[i].version ? p->depends[i].version : "");
        printf("\n");
    }
    if (p->nprovides) {
        printf("    %sprovides%s       :", C_DIM, C_OFF);
        for (int i = 0; i < p->nprovides; i++)
            printf(" %s%s%s", p->provides[i].name,
                   dep_op_str(p->provides[i].op),
                   p->provides[i].version ? p->provides[i].version : "");
        printf("\n");
    }
    if (p->nconflicts) {
        printf("    %sconflicts with%s :", C_DIM, C_OFF);
        for (int i = 0; i < p->nconflicts; i++)
            printf(" %s", p->conflicts[i].name);
        printf("\n");
    }
    if (p->nreplaces) {
        printf("    %sreplaces%s        :", C_DIM, C_OFF);
        for (int i = 0; i < p->nreplaces; i++)
            printf(" %s", p->replaces[i].name);
        printf("\n");
    }
    repo_free(r);
    return 0;
}

/* ---- command: stats ------------------------------------------------------- */

/* qsort() comparator for an array of const char* (never cast strcmp
 * directly: the pointer-indirection and calling conventions differ). */
static int cmp_str_ptr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int cmd_stats(const options *o) {
    repo *r = load_repo(o);
    long  total_csize = 0, total_isize = 0;
    int   with_provides = 0, multi_version = 0;
    int   n = repo_count(r);

    for (int i = 0; i < n; i++) {
        const package *p = repo_get(r, i);
        if (p->csize > 0) total_csize += p->csize;
        if (p->isize > 0) total_isize += p->isize;
        if (p->nprovides) with_provides++;
    }

    /* Multi-version names: sort the candidate names and count runs of length
     * > 1. (A per-name repo_lookup here would be O(n^2) on a 15k repo.) */
    const char **names = malloc((size_t)(n ? n : 1) * sizeof *names);
    if (names) {
        for (int i = 0; i < n; i++) names[i] = repo_get(r, i)->name;
        qsort(names, (size_t)n, sizeof *names, cmp_str_ptr);
        for (int i = 0; i < n; ) {
            int j = i + 1;
            while (j < n && strcmp(names[i], names[j]) == 0) j++;
            if (j - i > 1) multi_version++;
            i = j;
        }
        free(names);
    }

    printf("%sRepository%s\n", C_BOLD, C_OFF);
    printf("  candidates        : %d\n", n);
    printf("  multi-version     : %d\n", multi_version);
    printf("  providing virtuals: %d\n", with_provides);
    printf("  total download    : %s\n", hr_size((long)total_csize));
    printf("  total installed   : %s\n", hr_size((long)total_isize));
    repo_free(r);
    return 0;
}

/* ---- command: solve ------------------------------------------------------- */

/* Parse the request arguments ("vim", "gcc>=12", ...) into deps. Returns the
 * count; dies with a clear message on malformed input. */
static int cmd_solve(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "solve needs at least one package name");
    repo *r = load_repo(o);

    int   nreq = 0;
    dep  *reqs = parse_requests(argc, argv, &nreq);

    /* Reject names the repos simply do not have: nothing to optimise. */
    for (int i = 0; i < nreq; i++) {
        if (!repo_has_provider(r, reqs[i].name, reqs[i].op, reqs[i].version)) {
            fprintf(stderr, "%serror:%s nothing in the repositories provides "
                            "'%s%s%s'\n", C_RED, C_OFF, reqs[i].name,
                    dep_op_str(reqs[i].op), reqs[i].version ? reqs[i].version : "");
            repo_free(r);
            return 1;
        }
    }

    encoder *e = enc_new(r);
    if (!e) die(1, "out of memory");
    for (int i = 0; i < nreq; i++) enc_request(e, reqs[i].name, reqs[i].op,
                                               reqs[i].version, 0);

    printf("Resolving");
    for (int i = 0; i < nreq; i++)
        printf(" %s%s%s%s%s", C_CYAN, reqs[i].name, dep_op_str(reqs[i].op),
               reqs[i].version ? reqs[i].version : "", C_OFF);
    printf("  %s(%d candidates)%s\n\n", C_DIM, enc_ncandidates(e), C_OFF);

    double t0 = now_ms();
    int    rc = enc_solve(e);
    double dt = now_ms() - t0;

    if (rc != MAXSAT_OPTIMAL) {
        /* Distinguish "a request is individually impossible" from "the
         * requests fight each other" — with a small request list this costs
         * only a handful of extra solves and turns a bare UNSAT into an
         * explanation the user can act on. */
        fprintf(stderr, "%sno consistent system exists%s for that request "
                        "set.\n", C_RED, C_OFF);
        if (nreq <= 6) {
            for (int i = 0; i < nreq; i++) {
                encoder *one = enc_new(r);
                enc_request(one, reqs[i].name, reqs[i].op, reqs[i].version, 0);
                if (enc_solve(one) != MAXSAT_OPTIMAL)
                    fprintf(stderr, "  %s%s%s is uninstallable on its own "
                            "(its dependency closure cannot be satisfied)\n",
                            C_YELLOW, reqs[i].name, C_OFF);
                enc_free(one);
            }
            if (nreq > 1)
                fprintf(stderr, "  %s(if every request is individually "
                        "installable, they conflict with each other)%s\n",
                        C_DIM, C_OFF);
        }
        enc_free(e);
        repo_free(r);
        return 1;
    }

    /* ---- print the optimal plan ------------------------------------------ */

    /* Chosen packages, sorted by name for a stable, readable table. */
    int nchosen = 0;
    for (int i = 0; i < enc_ncandidates(e); i++)
        if (enc_chosen(e, i)) nchosen++;

    const package **plan = malloc((size_t)(nchosen ? nchosen : 1) * sizeof *plan);
    char           *why  = malloc((size_t)(nchosen ? nchosen : 1));
    if (!plan || !why) die(1, "out of memory");
    for (int i = 0, k = 0; i < enc_ncandidates(e); i++) {
        if (!enc_chosen(e, i)) continue;
        const package *p = enc_candidate(e, i);
        plan[k] = p;
        /* "requested" if this exact candidate satisfies one of the user's
         * requests; otherwise it was pulled in as a dependency. */
        why[k] = 'D';
        for (int j = 0; j < nreq; j++)
            if (pkg_provides(p, reqs[j].name, reqs[j].op, reqs[j].version)) {
                why[k] = 'R';
                break;
            }
        k++;
    }
    /* insertion sort by name (n is small: a transaction, not a repo) */
    for (int i = 1; i < nchosen; i++) {
        const package *pv = plan[i]; char wv = why[i];
        int j = i - 1;
        while (j >= 0 && strcmp(plan[j]->name, pv->name) > 0) {
            plan[j + 1] = plan[j]; why[j + 1] = why[j]; j--;
        }
        plan[j + 1] = pv; why[j + 1] = wv;
    }

    printf("%s%-24s %-16s %10s %10s  %s%s\n", C_BOLD,
           "Package", "Version", "Download", "Installed", "Why", C_OFF);
    long dl = 0, inst = 0;
    for (int i = 0; i < nchosen; i++) {
        const package *p = plan[i];
        const char *tag = why[i] == 'R' ? "requested" : "dependency";
        const char *tagc = why[i] == 'R' ? C_GREEN : C_DIM;
        printf("%s%-24s%s %-16s %10s %10s  %s%s%s\n",
               C_CYAN, p->name, C_OFF, p->version,
               p->csize > 0 ? hr_size(p->csize) : "-",
               p->isize > 0 ? hr_size(p->isize) : "-",
               tagc, tag, C_OFF);
        if (p->csize > 0) dl += p->csize;
        if (p->isize > 0) inst += p->isize;
    }

    printf("\n  %d package(s), %s to download, %s installed\n",
           nchosen, hr_size(dl), hr_size(inst));
    printf("  %soptimal cost: %ld (broken soft weight)%s\n", C_DIM,
           enc_cost(e), C_OFF);

    if (o->solver_stats) {
        sat_stats st;
        enc_stats(e, &st);
        double per_dec = st.decisions ? (double)st.propagations / st.decisions
                                      : 0.0;
        printf("\n  %sSolver statistics%s\n", C_BOLD, C_OFF);
        printf("    BCP propagations  : %ld  (%.1f per decision)\n",
               st.propagations, per_dec);
        printf("    conflicts         : %ld\n", st.conflicts);
        printf("    decisions         : %ld\n", st.decisions);
        printf("    learned clauses   : %ld\n", st.learnts);
        printf("    restarts          : %ld\n", st.restarts);
        printf("    VSIDS decay       : %.3f\n", (double)st.vsids_decay);
        printf("    IHS core rounds   : %ld\n", enc_rounds(e));
        printf("    requests satisfied: %d/%d\n", enc_satisfied(e), nreq);
        printf("    total time        : %.2f ms\n", dt);
    }

    free(plan); free(why);
    enc_free(e);
    repo_free(r);
    for (int i = 0; i < nreq; i++) { free(reqs[i].name); free(reqs[i].version); }
    free(reqs);
    return 0;
}

/* ---- command: dump-cnf ---------------------------------------------------- */

static int cmd_dump_cnf(const options *o, int argc, char **argv) {
    if (argc == 0) die(2, "dump-cnf needs at least one package name");
    repo *r = load_repo(o);

    int  nreq = 0;
    dep *reqs = parse_requests(argc, argv, &nreq);
    encoder *e = enc_new(r);
    if (!e) die(1, "out of memory");
    for (int i = 0; i < nreq; i++)
        if (!enc_request(e, reqs[i].name, reqs[i].op, reqs[i].version, 0))
            die(1, "nothing provides '%s'", reqs[i].name);

    FILE *fp = stdout;
    if (o->out_file) {
        fp = fopen(o->out_file, "w");
        if (!fp) die(1, "cannot write '%s'", o->out_file);
    }

    int n = enc_dump_cnf(e, fp);
    if (n < 0) die(1, "failed to build the instance");
    if (o->out_file) {
        fclose(fp);
        printf("%swrote %d clauses to %s%s\n", C_GREEN, n, o->out_file, C_OFF);
    }

    enc_free(e);
    repo_free(r);
    for (int i = 0; i < nreq; i++) { free(reqs[i].name); free(reqs[i].version); }
    free(reqs);
    return 0;
}

/* ---- command: doctor ------------------------------------------------------ */

static int cmd_doctor(const options *o) {
    int problems = 0;

    printf("%sPlanet doctor%s\n\n", C_BOLD, C_OFF);

    /* 1. libraries */
    printf("  %-26s %s\n", "libcurl", curl_version());
    printf("  %-26s %s\n", "libarchive", archive_version_string());

    /* 2. cache directory */
    if (access(o->cache_dir, W_OK) == 0)
        printf("  %-26s %sok%s (%s)\n", "cache directory", C_GREEN, C_OFF,
               o->cache_dir);
    else {
        printf("  %-26s %smissing/unwritable%s (%s)\n", "cache directory",
               C_RED, C_OFF, o->cache_dir);
        problems++;
    }

    /* 3. databases parse? */
    for (int i = 0; i < g_nrepos; i++) {
        char *path = db_cache_path(o->cache_dir, g_repos[i].name);
        if (access(path, R_OK) != 0) {
            printf("  %-26s %snot cached%s (%s)\n", g_repos[i].name, C_YELLOW,
                   C_OFF, path);
            problems++;
        } else {
            repo *tmp = repo_new();
            archrepo_stats st;
            char err[256] = {0};
            if (archrepo_load_file(tmp, path, g_repos[i].url, &st, err,
                                   sizeof err) == 0)
                printf("  %-26s %sok%s (%d packages, %d malformed)\n",
                       g_repos[i].name, C_GREEN, C_OFF, st.packages,
                       st.malformed);
            else {
                printf("  %-26s %scorrupt:%s %s\n", g_repos[i].name, C_RED,
                       C_OFF, err);
                problems++;
            }
            repo_free(tmp);
        }
        free(path);
    }

    /* 4. target root */
    if (access(o->root, W_OK) != 0) {
        printf("  %-26s %snot writable%s (%s — use sudo for system changes)\n",
               "target root", C_YELLOW, C_OFF, o->root);
    } else {
        printf("  %-26s %sok%s (%s)\n", "target root", C_GREEN, C_OFF, o->root);
    }

    printf("\n  %s\n", problems ? "problems found — see above"
                                : "everything looks healthy");
    return problems ? 1 : 0;
}

/* ---- help ----------------------------------------------------------------- */

static void usage(FILE *fp) {
    fprintf(fp,
"%sNeptumium Planet%s %s — dependency resolution you can prove optimal\n"
"\n"
"%sUsage:%s planet [options] <command> [arguments]\n"
"\n"
"%sCommands:%s\n"
"  sync                 download the repository databases into the cache\n"
"  install <name...>    download, verify, extract and record packages\n"
"  remove <name...>     remove packages and their files\n"
"  upgrade [name...]    move installed packages to the newest feasible set\n"
"  autoremove           remove packages nothing else requires\n"
"  lock                 write the installed system state to a lock file\n"
"  search <term>        find packages by name or description\n"
"  info <name>          show a package in full detail\n"
"  stats                summarise the loaded repository\n"
"  solve <name...>      compute the optimal package set for a request\n"
"                       (accepts constraints: 'vim>=9' 'gcc=15.2')\n"
"  dump-cnf <name...>   export the MaxSAT instance (WCNF) for a request\n"
"  doctor               check cache, databases and environment\n"
"  help | version       this text / the version number\n"
"\n"
"%sOptions:%s\n"
"  -r, --root <dir>      operate on <dir> as the target system root\n"
"  -c, --cache-dir <dir> where databases live (default: ~/.cache/planet)\n"
"      --repo <name=URL> add or override a repository (repeatable)\n"
"      --demo            use the built-in example repository (no network)\n"
"  -n, --dry-run         report what would happen, change nothing\n"
"  -y, --yes             assume yes; skip confirmation prompts\n"
"      --no-hooks        do not run ALPM/.INSTALL hooks during the transaction\n"
"  -S, --solver-stats    print solver statistics (BCP rate, learned clauses,\n"
"                        VSIDS decay, timing)\n"
"  -o, --output <file>   write output to <file> (dump-cnf)\n"
"  -v, --verbose         explain each step\n"
"  -h, --help            this text\n"
"  -V, --version         print the version\n"
"\n"
"%sExamples:%s\n"
"  planet sync\n"
"  planet search editor\n"
"  planet solve git vim            # the optimal set for both, exactly\n"
"  planet solve git -S             # ...with solver statistics\n"
"  planet dump-cnf git -o git.wcnf # reproduce the instance elsewhere\n",
        C_BOLD, C_OFF, PLANET_VERSION,
        C_BOLD, C_OFF, C_BOLD, C_OFF, C_BOLD, C_OFF, C_BOLD, C_OFF);
}

/* ---- argument parsing ----------------------------------------------------- */

/* Default cache: $XDG_CACHE_HOME/planet, else ~/.cache/planet, else a local
 * directory. Follows the XDG base-directory convention, like other tools. */
static char *default_cache_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        size_t n = strlen(xdg) + 16;
        char *p = malloc(n);
        if (p) snprintf(p, n, "%s/planet", xdg);
        return p;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        size_t n = strlen(home) + 24;
        char *p = malloc(n);
        if (p) snprintf(p, n, "%s/.cache/planet", home);
        return p;
    }
    return strdup(".planet-cache");
}

int main(int argc, char **argv) {
    options o = { .root = "/", .cache_dir = NULL, .out_file = NULL,
                  .dry_run = 0, .verbose = 0, .solver_stats = 0, .yes = 0,
                  .demo = 0 };

    /* First pass: global options anywhere on the command line, leaving the
     * command and its arguments in place. This lets users write either
     * `planet solve git -S` or `planet -S solve git`. */
    char *rest[128];
    int   nrest = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-h") || !strcmp(a, "--help"))    { usage(stdout); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("planet %s (Neptumium)\n", PLANET_VERSION); return 0; }
        else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) o.dry_run = 1;
        else if (!strcmp(a, "-y") || !strcmp(a, "--yes"))      o.yes = 1;
        else if (!strcmp(a, "--no-hooks"))           hooks_set_enabled(0);
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) o.verbose = 1;
        else if (!strcmp(a, "-S") || !strcmp(a, "--solver-stats")) o.solver_stats = 1;
        else if (!strcmp(a, "--demo"))                        o.demo = 1;
        else if (!strcmp(a, "-r") || !strcmp(a, "--root")) {
            if (++i >= argc) die(2, "%s needs a directory", a);
            o.root = argv[i];
        } else if (!strcmp(a, "-c") || !strcmp(a, "--cache-dir")) {
            if (++i >= argc) die(2, "%s needs a directory", a);
            o.cache_dir = argv[i];
        } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            if (++i >= argc) die(2, "%s needs a file name", a);
            o.out_file = argv[i];
        } else if (!strcmp(a, "--repo")) {
            if (++i >= argc) die(2, "--repo needs name=URL");
            char *eq = strchr(argv[i], '=');
            if (!eq) die(2, "--repo wants the form name=URL");
            *eq = '\0';
            /* replace an existing repo of the same name, else append */
            int replaced = 0;
            for (int k = 0; k < g_nrepos; k++)
                if (!strcmp(g_repos[k].name, argv[i])) {
                    free(g_repos[k].url);
                    g_repos[k].url = strdup(eq + 1);
                    replaced = 1;
                    break;
                }
            if (!replaced) repo_src_add(argv[i], eq + 1);
            *eq = '=';
        } else {
            if (nrest == 128) die(2, "too many arguments");
            rest[nrest++] = argv[i];
        }
    }

    /* Colour only on a real terminal, and honour NO_COLOR. */
    g_color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
    g_yes = o.yes;
    /* default_cache_dir() returns an owned malloc'd string; remember it so we
     * can free it at exit (argv-provided paths are borrowed, not owned). */
    char *own_cache_dir = NULL;
    if (!o.cache_dir) { own_cache_dir = default_cache_dir(); o.cache_dir = own_cache_dir; }
    if (g_nrepos == 0) repos_default();

    if (nrest == 0) { usage(stdout); return 0; }
    const char *cmd = rest[0];

    int rc = 0;
    if      (!strcmp(cmd, "help"))    usage(stdout);
    else if (!strcmp(cmd, "version")) printf("planet %s (Neptumium)\n", PLANET_VERSION);
    else if (!strcmp(cmd, "sync"))    rc = cmd_sync(&o);
    else if (!strcmp(cmd, "install"))  rc = cmd_install(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "remove"))   rc = cmd_remove(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "upgrade"))  rc = cmd_upgrade(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "update"))   rc = cmd_upgrade(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "autoremove")) rc = cmd_autoremove(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "lock"))     rc = cmd_lock(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "search")) {
        if (nrest < 2) die(2, "search needs a term");
        rc = cmd_search(&o, rest[1]);
    }
    else if (!strcmp(cmd, "info")) {
        if (nrest < 2) die(2, "info needs a package name");
        rc = cmd_info(&o, rest[1]);
    }
    else if (!strcmp(cmd, "stats"))   rc = cmd_stats(&o);
    else if (!strcmp(cmd, "solve"))   rc = cmd_solve(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "dump-cnf"))rc = cmd_dump_cnf(&o, nrest - 1, rest + 1);
    else if (!strcmp(cmd, "doctor"))  rc = cmd_doctor(&o);
    else die(2, "unknown command '%s' (try 'planet help')", cmd);

    free(own_cache_dir);
    return rc;
}
