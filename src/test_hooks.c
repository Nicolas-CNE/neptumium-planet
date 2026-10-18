/*
 * test_hooks.c — lock down the ALPM hooks layer.
 * ===========================================================================
 *
 * Hooks run arbitrary shell code during transactions, so a parsing mistake is
 * either silent (a hook that should fire does nothing — the failure mode that
 * leaves a system subtly broken) or too eager (a hook fires on the wrong
 * packages). These tests pin both directions:
 *
 *   - a synthetic .pkg.tar.zst carrying a .INSTALL script with all six
 *     phases; each phase appends its own name to a log, so the test proves
 *     that exactly the right function ran;
 *   - a package WITHOUT .INSTALL must be a no-op, not an error;
 *   - repository .hook files: operation matching, glob target matching, When
 *     keyword matching, and the "no hooks directory" case;
 *   - ./ prefixed archive members still resolve (see entry_name()).
 */
#define _GNU_SOURCE  /* strdup() under -std=c11 */

#include "hooks.h"
#include "pkgfile.h"
#include "sha256.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}

static char g_dir[256];        /* scratch root for this test run   */
static char g_payload[512];    /* synthetic .pkg.tar.zst           */
static char g_log[512];        /* where the hook scripts append to */
static char g_cache[512];      /* package cache override           */

static void rmrf(const char *p) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd) != 0) { /* best effort */ }
}

/* Build a .pkg.tar.zst containing a .INSTALL script with all six phases, each
 * appending its own name to `logpath`. Returns the payload path. */
static int build_payload(const char *out, const char *logpath, int with_install) {
    struct archive *a = archive_write_new();
    if (!a) return -1;
    archive_write_set_format_pax(a);
    archive_write_add_filter_zstd(a);

    if (archive_write_open_filename(a, out) != ARCHIVE_OK) {
        archive_write_free(a); return -1;
    }

    /* A payload file, so extraction has something to do. */
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, "usr/bin/hooktest");
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0755);
    archive_entry_set_size(e, 5);
    archive_write_header(a, e);
    archive_write_data(a, "hello", 5);
    archive_entry_free(e);

    if (with_install) {
        char script[4096];
        snprintf(script, sizeof script,
                 "pre_install()  { echo pre_install  >> %s; }\n"
                 "post_install() { echo post_install >> %s; }\n"
                 "pre_upgrade()  { echo pre_upgrade  >> %s; }\n"
                 "post_upgrade() { echo post_upgrade >> %s; }\n"
                 "pre_remove()   { echo pre_remove   >> %s; }\n"
                 "post_remove()  { echo post_remove  >> %s; }\n",
                 logpath, logpath, logpath, logpath, logpath, logpath);

        e = archive_entry_new();
        /* Deliberately "./"-prefixed: some archives do this and the extractor
         * must still recognise the .INSTALL entry. */
        archive_entry_set_pathname(e, "./.INSTALL");
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0755);
        archive_entry_set_size(e, strlen(script));
        archive_write_header(a, e);
        archive_write_data(a, script, strlen(script));
        archive_entry_free(e);
    }

    archive_write_close(a);
    archive_write_free(a);
    return 0;
}

/* Read the hook log into a buffer (empty string if absent). */
static void read_log(char *out, size_t n) {
    out[0] = '\0';
    FILE *f = fopen(g_log, "r");
    if (!f) return;
    size_t used = 0;
    char line[128];
    while (fgets(line, sizeof line, f) && used + 1 < n) {
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        used += (size_t)snprintf(out + used, n - used, "%s%s",
                                 used ? "," : "", line);
    }
    fclose(f);
}

/* Build a package and seed the package cache with the synthetic payload under
 * its real digest, so pkgfile_fetch resolves it from disk and never touches
 * the (nonexistent) origin URL. */
static package *pkg_for(const char *name) {
    package *p = pkg_new(name, "1.0-1", "x86_64");
    if (!p) return NULL;

    FILE *f = fopen(g_payload, "rb");
    if (!f) return p;
    sha256_ctx c; sha256_init(&c);
    unsigned char buf[8192]; size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, got);
    rewind(f);
    uint8_t d[32]; char hex[65];
    sha256_final(&c, d); sha256_hex(d, hex);

    p->sha256   = strdup(hex);
    p->origin   = strdup("file:///nonexistent/");  /* only reached on a miss */
    p->filename = strdup("hooktest.pkg");

    /* <cache>/pkgs/<sha>.pkg — mirror pkgfile_fetch's cache layout. */
    char pkgs[1024], dst[1200];
    snprintf(pkgs, sizeof pkgs, "%s/pkgs", g_cache);
    snprintf(dst, sizeof dst, "%s/%s.pkg", pkgs, hex);
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", pkgs);
    if (system(cmd) != 0) { fclose(f); return p; }
    FILE *out = fopen(dst, "wb");
    if (out) {
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
    }
    fclose(f);
    return p;
}

static void test_install_script_phases(void) {
    package *p = pkg_for("hooktest");
    check("hooks: package built", p != NULL);
    if (!p) return;

    struct { install_phase ph; const char *fn; const char *expect; } cases[] = {
        { IP_PRE_INSTALL,  "pre_install",  "pre_install"  },
        { IP_POST_INSTALL, "post_install", "post_install" },
        { IP_PRE_UPGRADE,  "pre_upgrade",  "pre_upgrade"  },
        { IP_POST_UPGRADE, "post_upgrade", "post_upgrade" },
        { IP_PRE_REMOVE,   "pre_remove",   "pre_remove"   },
        { IP_POST_REMOVE,  "post_remove",  "post_remove"  },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        remove(g_log);
        int rc = hooks_run_install_script(p, g_dir, cases[i].ph, NULL);
        char log[512];
        read_log(log, sizeof log);

        char label[128];
        snprintf(label, sizeof label, "install script: %s runs", cases[i].fn);
        check(label, rc == 0 && strcmp(log, cases[i].expect) == 0);

        snprintf(label, sizeof label, "install script: %s only, not others",
                 cases[i].fn);
        check(label, strncmp(log, cases[i].expect, strlen(cases[i].expect)) == 0 &&
                     strstr(log, "post_") == (strstr(cases[i].expect, "post_")
                         ? strstr(log, "post_") : NULL));
    }
    pkg_free(p);
}

static void test_package_without_install(void) {
    /* Rebuild the payload with no .INSTALL; every phase must be a clean
     * no-op rather than an error. */
    build_payload(g_payload, g_log, 0);
    package *p = pkg_for("hooktest");
    if (!p) { check("hooks: no-install package built", 0); return; }

    remove(g_log);
    int rc = hooks_run_install_script(p, g_dir, IP_POST_INSTALL, NULL);
    char log[512]; read_log(log, sizeof log);

    check("hooks: package without .INSTALL is a no-op", rc == 0 && log[0] == '\0');
    pkg_free(p);

    /* Restore the payload WITH the script for any later test. */
    build_payload(g_payload, g_log, 1);
}

static void write_hook(const char *name, const char *body) {
    char path[1024];
    snprintf(path, sizeof path, "%s/etc/planet/hooks/%s", g_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

static void test_repo_hooks(void) {
    char hdir[1024];
    snprintf(hdir, sizeof hdir, "%s/etc/planet/hooks", g_dir);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", hdir);
    if (system(cmd) != 0) { check("hooks: can create hook dir", 0); return; }

    char logarg[600];
    snprintf(logarg, sizeof logarg, "echo %s.$(date +%%s%%N) >> %s", "%s", g_log);

    /* (a) targeted Install hook for glibc only */
    char body[1400];
    snprintf(body, sizeof body,
             "[Trigger]\nOperation = Install\nType = Package\nTarget = glibc*\n\n"
             "[Action]\nDescription = glibc only\nWhen = PostInstall\n"
             "Exec = echo glibc-post >> %s\n", g_log);
    write_hook("a.hook", body);

    /* (b) catch-all Install hook at transaction level */
    snprintf(body, sizeof body,
             "[Trigger]\nOperation = Install\nType = Package\nTarget = *\n\n"
             "[Action]\nDescription = any package\nWhen = PostTransaction\n"
             "Exec = echo any-posttx >> %s\n", g_log);
    write_hook("b.hook", body);

    /* (c) Remove hook for bash */
    snprintf(body, sizeof body,
             "[Trigger]\nOperation = Remove\nType = Package\nTarget = bash\n\n"
             "[Action]\nDescription = bash removal\nWhen = PostRemove\n"
             "Exec = echo bash-postrm >> %s\n", g_log);
    write_hook("c.hook", body);

    /* (d) a hook whose target matches nothing we install */
    snprintf(body, sizeof body,
             "[Trigger]\nOperation = Install\nType = Package\nTarget = zzz*\n\n"
             "[Action]\nDescription = never\nWhen = PostInstall\n"
             "Exec = echo NEVER >> %s\n", g_log);
    write_hook("d.hook", body);

    /* Install glibc: (a) and (b) fire, (c) and (d) must not. */
    remove(g_log);
    package *g = pkg_new("glibc", "2.40-3", "x86_64");
    const package *pk[1] = { g };
    int rc = hooks_run_repo(g_dir, "Install", HP_POST_INSTALL, pk, 1);
    char log[512]; read_log(log, sizeof log);
    check("repo hook: targeted Install/PostInstall fired",
          rc == 0 && strstr(log, "glibc-post") != NULL);
    check("repo hook: non-matching target did NOT fire",
          strstr(log, "NEVER") == NULL);

    remove(g_log);
    rc = hooks_run_repo(g_dir, "Install", HP_POST_TRANSACTION, pk, 1);
    read_log(log, sizeof log);
    check("repo hook: PostTransaction fired",
          rc == 0 && strstr(log, "any-posttx") != NULL);

    /* Remove bash: (c) fires. */
    remove(g_log);
    package *b = pkg_new("bash", "5.3-20-1", "x86_64");
    const package *pb[1] = { b };
    rc = hooks_run_repo(g_dir, "Remove", HP_POST_REMOVE, pb, 1);
    read_log(log, sizeof log);
    check("repo hook: Remove/PostRemove fired for its own target",
          rc == 0 && strstr(log, "bash-postrm") != NULL);

    /* Wrong operation must not fire the Install hooks. */
    remove(g_log);
    rc = hooks_run_repo(g_dir, "Remove", HP_POST_INSTALL, pb, 1);
    read_log(log, sizeof log);
    check("repo hook: wrong Operation does not fire", log[0] == '\0');

    pkg_free(g); pkg_free(b);
}

static void test_no_hook_dir(void) {
    char other[256];
    snprintf(other, sizeof other, "%.200s-nohooks", g_dir);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", other, other);
    if (system(cmd) != 0) { check("hooks: scratch root ok", 0); return; }

    package *p = pkg_new("glibc", "2.40-3", "x86_64");
    const package *pk[1] = { p };
    int rc = hooks_run_repo(other, "Install", HP_POST_INSTALL, pk, 1);
    check("repo hook: missing hooks directory is a clean no-op", rc == 0);
    pkg_free(p);
    rmrf(other);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/planet-hooks-test-%ld", (long)getpid());
    snprintf(g_log, sizeof g_log, "%s.log", g_dir);
    snprintf(g_payload, sizeof g_payload, "%s/hooktest.pkg.tar.zst", g_dir);
    snprintf(g_cache, sizeof g_cache, "%s-cache", g_dir);

    rmrf(g_dir); rmrf(g_cache);
    { char cmd[1200]; snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s'", g_dir, g_cache); system(cmd); }

    pkgfile_cache_dir_set(g_cache);
    hooks_set_enabled(1);

    check("hooks: synthetic payload built",
          build_payload(g_payload, g_log, 1) == 0);

    test_install_script_phases();
    test_package_without_install();
    test_repo_hooks();
    test_no_hook_dir();

    /* --no-hooks must disable everything. */
    hooks_set_enabled(0);
    package *p = pkg_for("hooktest");
    remove(g_log);
    int rc = hooks_run_install_script(p, g_dir, IP_POST_INSTALL, NULL);
    char log[512]; read_log(log, sizeof log);
    check("hooks: --no-hooks disables package scripts", rc == 0 && log[0] == '\0');
    pkg_free(p);

    rmrf(g_dir); rmrf(g_cache);
    remove(g_log);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}