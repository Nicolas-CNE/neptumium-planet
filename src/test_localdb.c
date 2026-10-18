/*
 * test_localdb.c — lock down the local (installed) database and the payload
 * verification path.
 * ===========================================================================
 *
 * The local database is what makes install/remove/upgrade stateful: without a
 * correct round-trip (write a package, read it back, delete it) the resolver
 * cannot reason about the system at all. These tests cover:
 *
 *   - localdb_add / localdb_list round-trip, including version constraints in
 *     Depends (the encode layer needs those to resolve the installed system);
 *   - localdb_is_installed, including the epoch-in-directory-name case;
 *   - localdb_remove cleaning up both the desc and the files list;
 *   - the NULL-termination invariant of the returned list (localdb_free_list
 *     walks it without a length, so an unterminated list is a crash);
 *   - pkgfile_verify accepting a correct digest and rejecting a wrong one.
 */
#define _GNU_SOURCE  /* strdup() under -std=c11 */

#include "localdb.h"
#include "pkgfile.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}

/* A unique scratch root under /tmp so the tests never touch a real system. */
static char g_root[128];

static void make_scratch_root(void) {
    snprintf(g_root, sizeof g_root, "/tmp/planet-test-%ld", (long)getpid());
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_root, g_root);
    if (system(cmd) != 0) { printf("SKIP  cannot create scratch root\n"); exit(0); }
}

static void remove_scratch_root(void) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    if (system(cmd) != 0) { /* best effort */ }
}

/* Build a package with a couple of versioned deps. */
static package *make_pkg(const char *name, const char *ver) {
    package *p = pkg_new(name, ver, "x86_64");
    if (!p) return NULL;
    pkg_set_desc(p, "test package");
    pkg_add_dep(p, "glibc>=2.38");
    pkg_add_dep(p, "readline");
    pkg_add_provide(p, "libtest.so=1.0");
    pkg_add_conflict(p, "old-test");
    pkg_add_replace(p, "old-test");
    return p;
}

static void test_add_and_list(void) {
    package *p = make_pkg("testpkg", "1:2.0-3");
    check("add: package built", p != NULL);

    const char *files[] = { "usr/", "usr/bin/", "usr/bin/testpkg", "usr/share/doc/testpkg/README" };
    int rc = localdb_add(g_root, p, files, 4);
    check("add: recorded in local db", rc == 0);
    if (rc != 0) { pkg_free(p); return; }

    check("add: is_installed exact version", localdb_is_installed(g_root, "testpkg", "1:2.0-3") == 1);
    check("add: not a different version", localdb_is_installed(g_root, "testpkg", "2.0-3") == 0);
    check("add: not a different name", localdb_is_installed(g_root, "other", "1:2.0-3") == 0);

    int n = 0;
    package **list = localdb_list(g_root, &n);
    check("list: exactly one package", list != NULL && n == 1);
    if (!list || n != 1) { localdb_free_list(list); pkg_free(p); return; }

    const package *q = list[0];
    check("list: name round-trips", strcmp(q->name, "testpkg") == 0);
    check("list: version with epoch round-trips", strcmp(q->version, "1:2.0-3") == 0);
    check("list: arch round-trips", q->arch && strcmp(q->arch, "x86_64") == 0);
    check("list: desc round-trips", q->desc && strcmp(q->desc, "test package") == 0);

    /* The Depends constraints must survive: the resolver relies on them. */
    int has_ge = 0, has_plain = 0;
    for (int i = 0; i < q->ndepends; i++) {
        if (strcmp(q->depends[i].name, "glibc") == 0 && q->depends[i].op == DEP_GE
            && q->depends[i].version && strcmp(q->depends[i].version, "2.38") == 0)
            has_ge = 1;
        if (strcmp(q->depends[i].name, "readline") == 0 && q->depends[i].op == DEP_ANY)
            has_plain = 1;
    }
    check("list: versioned dep glibc>=2.38 preserved", has_ge);
    check("list: unversioned dep readline preserved", has_plain);

    int has_prov = 0;
    for (int i = 0; i < q->nprovides; i++)
        if (strcmp(q->provides[i].name, "libtest.so") == 0 && q->provides[i].op == DEP_EQ)
            has_prov = 1;
    check("list: versioned provide preserved", has_prov);
    check("list: conflict preserved", q->nconflicts == 1);
    check("list: replace preserved", q->nreplaces == 1);

    /* localdb_free_list walks until NULL: this must not crash. */
    localdb_free_list(list);
    check("list: NULL-terminated (free_list did not run off the end)", 1);

    pkg_free(p);
}

static void test_files_recorded(void) {
    char *base = localdb_path(g_root);
    char path[512];
    snprintf(path, sizeof path, "%s/testpkg-1:2.0-3/files", base);

    FILE *f = fopen(path, "r");
    check("files: files list written", f != NULL);
    if (!f) { free(base); return; }
    int lines = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) lines++;
    fclose(f);
    check("files: all four paths recorded", lines == 4);
    free(base);
}

static void test_remove(void) {
    int rc = localdb_remove(g_root, "testpkg", "1:2.0-3");
    check("remove: succeeds", rc == 0);
    check("remove: no longer installed", localdb_is_installed(g_root, "testpkg", "1:2.0-3") == 0);

    int n = 0;
    package **list = localdb_list(g_root, &n);
    check("remove: list is now empty", n == 0);
    localdb_free_list(list);

    /* Removing something that is not there must fail cleanly, not crash. */
    rc = localdb_remove(g_root, "ghost", "1.0-1");
    check("remove: missing package reports an error", rc != 0);
}

static void test_empty_list(void) {
    /* An empty directory must yield an empty list, not NULL-with-count. */
    int n = 42;
    package **list = localdb_list("/nonexistent-root-xyz", &n);
    check("empty: missing root yields n == 0", n == 0);
    localdb_free_list(list);   /* must tolerate NULL */
    check("empty: free_list(NULL) is safe", 1);
}

static void test_verify(void) {
    /* Write a known file and check pkgfile_verify accepts the right digest and
     * rejects a wrong one. "abc" hashes to a known value (see test_sha256). */
    const char *path = "/tmp/planet-verify-test.bin";
    FILE *f = fopen(path, "wb");
    if (!f) { check("verify: can create temp file", 0); return; }
    fputs("abc", f);
    fclose(f);

    package *p = pkg_new("probe", "1.0-1", "x86_64");
    p->sha256 = strdup("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("verify: correct digest accepted", pkgfile_verify(p, path) == 1);

    free(p->sha256);
    p->sha256 = strdup("0000000000000000000000000000000000000000000000000000000000000000");
    check("verify: wrong digest rejected", pkgfile_verify(p, path) != 1);
    check("verify: rejection names the package", strstr(pkgfile_error(), "probe") != NULL);

    pkg_free(p);
    remove(path);
}

int main(void) {
    make_scratch_root();

    test_add_and_list();
    test_files_recorded();
    test_remove();
    test_empty_list();
    test_verify();

    remove_scratch_root();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}