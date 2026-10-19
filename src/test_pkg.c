/*
 * test_pkg.c — lock down Arch-style version ordering and the package model.
 * ==========================================================================
 *
 * Version comparison is the most error-prone component of a package manager:
 * an incorrect "1.10 > 1.9" or tilde ordering silently installs the wrong
 * version. Every ordering rule exercised below is the rpm/alpm rule that Arch
 * itself relies on, and each is pinned with a concrete case before the
 * encoding layer is allowed to build on top of it.
 */
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}
/* assert ver_cmp(a,b) has the expected sign (-1, 0, +1) */
static void check_ver(const char *a, const char *b, int expect, const char *label) {
    int c = ver_cmp(a, b);
    int sign = c < 0 ? -1 : (c > 0 ? 1 : 0);
    char buf[256];
    snprintf(buf, sizeof buf, "%s  %s %s %s", sign == expect ? "PASS" : "FAIL", label, a, sign < 0 ? "<" : (sign > 0 ? ">" : "="));
    check(buf, sign == expect);
}

static void test_ver_basic(void) {
    check_ver("1.0",    "1.0",     0, "equal");
    check_ver("1.0",    "1.1",    -1, "minor bump");
    check_ver("1.0",    "2.0",    -1, "major bump");
    check_ver("2.0",    "1.0",     1, "major bump rev");
}

static void test_ver_numeric_segments(void) {
    /* digit segments compare as INTEGERS, not lexically: 1.10 > 1.9 */
    check_ver("1.9",    "1.10",   -1, "numeric segments");
    check_ver("1.10",   "1.9",     1, "numeric segments rev");
    check_ver("2.0",    "10.0",   -1, "two-digit major");
    check_ver("1.0.9",  "1.0.10", -1, "numeric patch");
}

static void test_ver_extra_segments(void) {
    /* a longer version is newer once the shared prefix matches */
    check_ver("1.0",    "1.0.1",  -1, "extra segment");
    check_ver("1.0.1",  "1.0",     1, "extra segment rev");
    check_ver("1.0a",   "1.0",     1, "alpha tail outranks end");
}

/* Arch pins split packages to one another with a bare version, while the
 * provider is built with a pkgrel: systemd depends on "systemd-libs=262.1"
 * and the package is "systemd-libs-262.1-1". The pin names the source version,
 * not the build revision, so it must match across pkgrels. Treating it as an
 * exact full-version match made every split package unresolvable, which is how
 * "planet install systemd" came to report that no consistent system exists. */
static void test_ver_eq_ignores_bare_pkgrel(void) {
    check("eq: bare pin matches any pkgrel",
          ver_satisfies("262.1-1", DEP_EQ, "262.1") == 1);
    check("eq: bare pin matches a larger pkgrel",
          ver_satisfies("262.1-7", DEP_EQ, "262.1") == 1);
    check("eq: bare pin still rejects a different version",
          ver_satisfies("263.1-1", DEP_EQ, "262.1") == 0);
    check("eq: bare pin respects the epoch",
          ver_satisfies("1:262.1-1", DEP_EQ, "1:262.1") == 1);
    check("eq: bare pin rejects a different epoch",
          ver_satisfies("2:262.1-1", DEP_EQ, "1:262.1") == 0);
    /* A constraint that DOES carry a pkgrel keeps exact full-version meaning. */
    check("eq: full pin matches the same pkgrel",
          ver_satisfies("262.1-1", DEP_EQ, "262.1-1") == 1);
    check("eq: full pin rejects a different pkgrel",
          ver_satisfies("262.1-2", DEP_EQ, "262.1-1") == 0);
    /* Ordering operators keep comparing whole versions, pkgrel included. */
    check("ge: includes pkgrel in the comparison",
          ver_satisfies("262.1-2", DEP_GE, "262.1-1") == 1);
    check("ge: bare version still compares",
          ver_satisfies("262.2", DEP_GE, "262.1") == 1);
}

static void test_ver_tilde(void) {
    /* '~' sorts BELOW everything: pre-releases come first */
    check_ver("1.0~rc1", "1.0",   -1, "tilde pre-release");
    check_ver("1.0",     "1.0~rc1", 1, "tilde pre-release rev");
    check_ver("1.0~rc1", "1.0~rc2", -1, "tilde ordering");
    check_ver("1.0~rc2", "1.0~rc1",  1, "tilde ordering rev");
    check_ver("1.0~rc1", "1.0~1",  -1, "tilde vs tilde-num");
}

static void test_ver_epoch(void) {
    /* epoch (before ':') compares numerically and dominates everything */
    check_ver("2:1.0",  "1.5",     1, "epoch dominates");
    check_ver("1:1.0",  "2:0.5",  -1, "lower epoch loses");
    check_ver("1:1.0",  "1:1.0",   0, "same epoch equal");
    check_ver("1.0",    "0:1.0",   0, "absent epoch == 0");
    check_ver("3:0.1",  "2:9.9",   1, "epoch over big ver");
}

static void test_ver_release(void) {
    /* pkgrel (after the last '-') compares with the same rpmvercmp rules */
    check_ver("1.0-1",   "1.0-2",  -1, "release bump");
    check_ver("1.0-2",   "1.0-10", -1, "release numeric");
    check_ver("1.0",     "1.0-1",  -1, "no release < release 1");
    check_ver("1.0-1",   "1.0",     1, "release > no release");
    check_ver("1.0-1.1", "1.0-1",   1, "dotted release");
}

static void test_ver_satisfies(void) {
    check("satisfies ANY",        ver_satisfies("1.0", DEP_ANY, NULL));
    check("satisfies EQ",         ver_satisfies("1.2", DEP_EQ, "1.2"));
    check("satisfies not EQ",    !ver_satisfies("1.2", DEP_EQ, "1.3"));
    check("satisfies GE",         ver_satisfies("1.2", DEP_GE, "1.2"));
    check("satisfies GE higher",  ver_satisfies("2.0", DEP_GE, "1.2"));
    check("satisfies not GE",    !ver_satisfies("1.1", DEP_GE, "1.2"));
    check("satisfies GT not eq", !ver_satisfies("1.2", DEP_GT, "1.2"));
    check("satisfies LE",         ver_satisfies("1.0", DEP_LE, "1.2"));
    check("satisfies LT",         ver_satisfies("1.1", DEP_LT, "1.2"));
    check("satisfies not LT",    !ver_satisfies("1.2", DEP_LT, "1.2"));
    /* numeric-aware: 1.10 satisfies >= 1.9 */
    check("satisfies numeric GE", ver_satisfies("1.10", DEP_GE, "1.9"));
}

static void test_dep_parse(void) {
    dep d;
    check("parse unversioned", dep_parse(&d, "glibc")
          && strcmp(d.name, "glibc") == 0 && d.op == DEP_ANY && d.version == NULL);
    dep_free(&d);

    check("parse >=", dep_parse(&d, "gcc>=12")
          && strcmp(d.name, "gcc") == 0 && d.op == DEP_GE && strcmp(d.version, "12") == 0);
    dep_free(&d);

    check("parse =", dep_parse(&d, "bash=5.2-1")
          && strcmp(d.name, "bash") == 0 && d.op == DEP_EQ && strcmp(d.version, "5.2-1") == 0);
    dep_free(&d);

    check("parse <=", dep_parse(&d, "foo<=1.0")
          && d.op == DEP_LE && strcmp(d.version, "1.0") == 0);
    dep_free(&d);

    check("parse name with dash", dep_parse(&d, "linux-api-headers>=6.5")
          && strcmp(d.name, "linux-api-headers") == 0 && d.op == DEP_GE);
    dep_free(&d);

    dep bad;
    check("reject no name",       !dep_parse(&bad, "=1.0"));
    check("reject op no version", !dep_parse(&bad, "foo>"));
    check("reject empty",         !dep_parse(&bad, ""));
}

static void test_pkg_provides(void) {
    package *p = pkg_new("foo", "1.2-1", "x86_64");
    pkg_add_provide(p, "libfoo");                 /* unversioned virtual */
    pkg_add_provide(p, "barapi=2.0");             /* versioned virtual   */

    check("provides own name",      pkg_provides(p, "foo", DEP_ANY, NULL));
    check("provides own version GE", pkg_provides(p, "foo", DEP_GE, "1.0"));
    check("not own version GT self", !pkg_provides(p, "foo", DEP_GT, "1.2-1"));
    check("provides virtual any",   pkg_provides(p, "libfoo", DEP_ANY, NULL));
    check("virtual versioned fails", !pkg_provides(p, "libfoo", DEP_GE, "1.0"));
    check("provides barapi >=1.9",  pkg_provides(p, "barapi", DEP_GE, "1.9"));
    check("not barapi >=3",        !pkg_provides(p, "barapi", DEP_GE, "3.0"));
    check("unrelated name fails",   !pkg_provides(p, "baz", DEP_ANY, NULL));
    pkg_free(p);
}

static void test_repo(void) {
    repo *r = repo_new();
    package *a1 = pkg_new("alpha", "1.0-1", "x86_64");
    package *a2 = pkg_new("alpha", "1.5-1", "x86_64");
    package *a3 = pkg_new("alpha", "1.10-1", "x86_64");
    package *b1 = pkg_new("beta",  "2.0-1", "x86_64");
    pkg_add_dep(b1, "alpha>=1.5");                /* beta needs newer alpha */
    repo_add(r, a1); repo_add(r, a2); repo_add(r, a3); repo_add(r, b1);

    check("repo count", repo_count(r) == 4);
    check("lookup alpha any -> 3", repo_lookup_count(r, "alpha", DEP_ANY, NULL) == 3);
    check("lookup alpha >=1.5 -> 2", repo_lookup_count(r, "alpha", DEP_GE, "1.5") == 2);
    check("lookup alpha =1.0-1 -> 1", repo_lookup_count(r, "alpha", DEP_EQ, "1.0-1") == 1);
    check("newest alpha is 1.10-1",
          strcmp(repo_newest(r, "alpha", DEP_ANY, NULL)->version, "1.10-1") == 0);
    check("newest alpha >=1.5 is 1.10-1",
          strcmp(repo_newest(r, "alpha", DEP_GE, "1.5")->version, "1.10-1") == 0);
    check("has provider gamma? no", !repo_has_provider(r, "gamma", DEP_ANY, NULL));
    check("has provider beta? yes", repo_has_provider(r, "beta", DEP_ANY, NULL));

    /* beta's dependency alpha>=1.5 is satisfiable by a2/a3 */
    const package *out[8];
    int n = repo_lookup(r, "alpha", DEP_GE, "1.5", out, 8);
    check("lookup fills array", n == 2 && (out[0] == a2 || out[1] == a2));

    repo_free(r);
}

int main(void) {
    printf("== Planet package model tests ==\n");
    test_ver_basic();
    test_ver_numeric_segments();
    test_ver_extra_segments();
    test_ver_eq_ignores_bare_pkgrel();
    test_ver_tilde();
    test_ver_epoch();
    test_ver_release();
    test_ver_satisfies();
    test_dep_parse();
    test_pkg_provides();
    test_repo();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
