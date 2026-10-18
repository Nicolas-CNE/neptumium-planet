/*
 * cli_util.c — Neptumium Planet: small CLI helpers and repository loading.
 * =============================================================
 *
 * Shared by every command module: fatal errors, formatting, confirmation
 * prompts, the configured-repository list, and loading a repository (from the
 * cache, or the built-in demo set).
 */
#define _GNU_SOURCE  /* strcasestr(), strdup() under -std=c11 */

#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <archive.h>
#include <curl/curl.h>

#include "archrepo.h"
#include "localdb.h"
#include "pkgfile.h"
#include "state.h"

int g_color = 0;
int g_yes = 0;

repo_src g_repos[MAX_REPOS];
int      g_nrepos = 0;

void die(int code, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%splanet:%s ", C_RED, C_OFF);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(code);
}

double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

const char *hr_size(long bytes) {
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

int ask_yes(const char *prompt) {
    if (g_yes) return 1;
    printf("%s [y/N] ", prompt);
    fflush(stdout);
    char buf[16] = {0};
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

int confirm_transaction(const char *label, int dry_run,
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

void repo_src_add(const char *name, const char *url) {
    if (g_nrepos == MAX_REPOS) die(2, "too many repositories (max %d)", MAX_REPOS);
    g_repos[g_nrepos].name = strdup(name);
    g_repos[g_nrepos].url  = strdup(url);
    if (!g_repos[g_nrepos].name || !g_repos[g_nrepos].url)
        die(1, "out of memory");
    g_nrepos++;
}

void repos_default(void) {
    repo_src_add("core",  "https://geo.mirror.pkgbuild.com/core/os/x86_64/");
    repo_src_add("extra", "https://geo.mirror.pkgbuild.com/extra/os/x86_64/");
}

char *db_cache_path(const char *cache_dir, const char *name) {
    size_t n = strlen(cache_dir) + strlen(name) + 16;
    char  *p = malloc(n);
    if (!p) die(1, "out of memory");
    snprintf(p, n, "%s/%s.db.tar.gz", cache_dir, name);
    return p;
}

void build_demo_repo(repo *r) {
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

repo *load_repo(const options *o) {
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

int cmp_str_ptr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
