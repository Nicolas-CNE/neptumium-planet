/*
 * test_encode.c — verify the package -> MaxSAT encoding yields the right sets.
 * ===========================================================================
 *
 * Each scenario is small enough to reason about by hand, and we assert the
 * OPTIMAL cost, WHICH candidates were chosen, and how many requests were met.
 * This locks down the three objectives interacting correctly: requests
 * (must-install), footprint (avoid installing needlessly), and newest-version
 * preference — plus Depends/Conflicts/Provides/Replaces producing valid sets.
 */
#include "encode.h"
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}

/* helper: find candidate index for name@version in the encoder's repo order */
static int find_cand(const repo *r, const encoder *e, const char *name, const char *ver) {
    for (int i = 0; i < enc_ncandidates(e); i++) {
        const package *p = enc_candidate(e, i);
        if (strcmp(p->name, name) == 0 && strcmp(p->version, ver) == 0)
            return i;
    }
    (void)r;
    return -1;
}

/* (1) request A, A depends B -> both chosen, cost = 2 footprint units */
static void test_simple_dep(void) {
    repo *r = repo_new();
    package *a = pkg_new("a", "1.0-1", NULL);
    pkg_add_dep(a, "b");
    package *b = pkg_new("b", "1.0-1", NULL);
    repo_add(r, a); repo_add(r, b);

    encoder *e = enc_new(r);
    check("dep: request a accepted", enc_request(e, "a", DEP_ANY, NULL, 0));
    check("dep: solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);

    int ia = find_cand(r, e, "a", "1.0-1");
    int ib = find_cand(r, e, "b", "1.0-1");
    check("dep: a chosen",  enc_chosen(e, ia));
    check("dep: b chosen",  enc_chosen(e, ib));
    check("dep: cost == 2 (footprint a+b)", enc_cost(e) == 2);
    check("dep: request met", enc_satisfied(e) == 1);

    enc_free(e); repo_free(r);
}

/* (2) three versions of alpha -> the NEWEST is chosen, cost only footprint */
static void test_newest_wins(void) {
    repo *r = repo_new();
    repo_add(r, pkg_new("alpha", "1.0-1", NULL));
    repo_add(r, pkg_new("alpha", "1.5-1", NULL));
    repo_add(r, pkg_new("alpha", "1.10-1", NULL));   /* newest */

    encoder *e = enc_new(r);
    check("newest: request alpha accepted", enc_request(e, "alpha", DEP_ANY, NULL, 0));
    check("newest: solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);

    int i110 = find_cand(r, e, "alpha", "1.10-1");
    int i15  = find_cand(r, e, "alpha", "1.5-1");
    int i10  = find_cand(r, e, "alpha", "1.0-1");
    check("newest: 1.10 chosen",  enc_chosen(e, i110));
    check("newest: 1.5 not chosen", !enc_chosen(e, i15));
    check("newest: 1.0 not chosen", !enc_chosen(e, i10));
    check("newest: cost == 1 (footprint only)", enc_cost(e) == 1);

    enc_free(e); repo_free(r);
}

/* (3) A conflicts B, both requested -> exactly one installed, one wish broken */
static void test_conflict(void) {
    repo *r = repo_new();
    package *a = pkg_new("a", "1.0-1", NULL);
    pkg_add_conflict(a, "b");
    repo_add(r, a);
    repo_add(r, pkg_new("b", "1.0-1", NULL));

    encoder *e = enc_new(r);
    enc_request(e, "a", DEP_ANY, NULL, 0);
    enc_request(e, "b", DEP_ANY, NULL, 0);
    check("conflict: solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);

    /* installing one costs footprint 1 and breaks the other request (10000);
     * installing none breaks both requests (20000). So one is optimal. */
    check("conflict: exactly one chosen",
          enc_chosen(e, find_cand(r, e, "a", "1.0-1")) !=
          enc_chosen(e, find_cand(r, e, "b", "1.0-1")));
    check("conflict: cost == 10001", enc_cost(e) == 10001);
    check("conflict: 1 of 2 requests met", enc_satisfied(e) == 1);

    enc_free(e); repo_free(r);
}

/* (4) A depends on something absent -> A uninstallable, request broken */
static void test_uninstallable(void) {
    repo *r = repo_new();
    package *a = pkg_new("a", "1.0-1", NULL);
    pkg_add_dep(a, "missing");
    repo_add(r, a);

    encoder *e = enc_new(r);
    check("uninst: request a accepted (a exists)", enc_request(e, "a", DEP_ANY, NULL, 0));
    check("uninst: solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);
    check("uninst: a not chosen", !enc_chosen(e, find_cand(r, e, "a", "1.0-1")));
    check("uninst: cost == 10000 (request broken)", enc_cost(e) == 10000);
    check("uninst: 0 requests met", enc_satisfied(e) == 0);

    enc_free(e); repo_free(r);
}

/* (5) newest alpha needs a missing dep -> falls back to the newest FEASIBLE */
static void test_newest_feasible(void) {
    repo *r = repo_new();
    repo_add(r, pkg_new("alpha", "1.0-1", NULL));       /* feasible */
    package *a2 = pkg_new("alpha", "2.0-1", NULL);       /* depends missing */
    pkg_add_dep(a2, "missing");
    repo_add(r, a2);

    encoder *e = enc_new(r);
    enc_request(e, "alpha", DEP_ANY, NULL, 0);
    check("feasible: solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);

    int i20 = find_cand(r, e, "alpha", "2.0-1");
    int i10 = find_cand(r, e, "alpha", "1.0-1");
    check("feasible: 2.0 blocked", !enc_chosen(e, i20));
    check("feasible: 1.0 chosen",   enc_chosen(e, i10));
    /* cost = footprint of installing 1.0 at rank 1 = footprint(1) + version(100) */
    check("feasible: cost == 101", enc_cost(e) == 101);
    check("feasible: request met", enc_satisfied(e) == 1);

    enc_free(e); repo_free(r);
}

/* (6) bar depends on virtual libfoo, which only foo Provides */
static void test_provides(void) {
    repo *r = repo_new();
    package *foo = pkg_new("foo", "1.0-1", NULL);
    pkg_add_provide(foo, "libfoo");
    package *bar = pkg_new("bar", "1.0-1", NULL);
    pkg_add_dep(bar, "libfoo");
    repo_add(r, foo); repo_add(r, bar);

    encoder *e = enc_new(r);
    enc_request(e, "bar", DEP_ANY, NULL, 0);
    check("provides: solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);
    check("provides: bar chosen", enc_chosen(e, find_cand(r, e, "bar", "1.0-1")));
    check("provides: foo chosen (satisfies libfoo)", enc_chosen(e, find_cand(r, e, "foo", "1.0-1")));
    check("provides: cost == 2", enc_cost(e) == 2);

    enc_free(e); repo_free(r);
}

/* (7) bar Replaces oldbar -> installing bar forces oldbar out (non-coexist) */
static void test_replaces(void) {
    repo *r = repo_new();
    package *bar = pkg_new("bar", "2.0-1", NULL);
    pkg_add_replace(bar, "oldbar");
    repo_add(r, bar);
    repo_add(r, pkg_new("oldbar", "1.0-1", NULL));

    encoder *e = enc_new(r);
    enc_request(e, "bar", DEP_ANY, NULL, 0);
    check("replaces: solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);
    check("replaces: bar chosen", enc_chosen(e, find_cand(r, e, "bar", "2.0-1")));
    check("replaces: oldbar kept out", !enc_chosen(e, find_cand(r, e, "oldbar", "1.0-1")));

    enc_free(e); repo_free(r);
}

/* (8) request for a name that is simply not in the repo is rejected up front */
static void test_missing_request(void) {
    repo *r = repo_new();
    repo_add(r, pkg_new("a", "1.0-1", NULL));
    encoder *e = enc_new(r);
    check("missing: request rejected", !enc_request(e, "nothere", DEP_ANY, NULL, 0));
    enc_free(e); repo_free(r);
}

/* The encoder scopes candidates to the closure reachable from the registered
 * requests and hard requirements, so a package nobody depends on never becomes
 * a candidate. `planet remove` still has to be able to take such a package off
 * the system, which it does by asking enc_chosen_pkg() about the installed
 * package directly. Both halves of that contract are pinned here: the package
 * is genuinely absent from the candidate set, and enc_chosen_pkg() reports it
 * as unselected rather than crashing or reporting a stale answer. */
static void test_closure_excludes_unreferenced(void) {
    repo *r = repo_new();
    package *a = pkg_new("a", "1.0-1", NULL);
    pkg_add_dep(a, "b");
    package *b = pkg_new("b", "1.0-1", NULL);
    package *lonely = pkg_new("lonely", "1.0-1", NULL);   /* nothing refs it */
    repo_add(r, a); repo_add(r, b); repo_add(r, lonely);

    encoder *e = enc_new(r);
    enc_request(e, "a", DEP_ANY, NULL, 0);
    int rc = enc_solve(e);
    check("closure: solve optimal", rc == MAXSAT_OPTIMAL);
    check("closure: a chosen", enc_chosen_pkg(e, a));
    check("closure: b chosen via dep", enc_chosen_pkg(e, b));

    /* The whole point: `lonely` is outside the closure, so it is not a
     * candidate, and must read as unselected so removal picks it up. */
    int cand_lonely = find_cand(r, e, "lonely", "1.0-1");
    check("closure: unreferenced pkg is NOT a candidate", cand_lonely < 0);
    check("closure: unreferenced pkg reads as unselected",
          !enc_chosen_pkg(e, lonely));
    check("closure: candidate count restricted", enc_ncandidates(e) == 2);

    enc_free(e); repo_free(r);
}

/* A package that IS reachable must stay out of the way of removal: if it is
 * required, enc_chosen_pkg() must say so. Guards the other direction, where a
 * too-eager closure would silently drop a package that has to stay. */
static void test_closure_keeps_required(void) {
    repo *r = repo_new();
    package *keepme = pkg_new("keepme", "1.0-1", NULL);
    package *dep = pkg_new("dep", "1.0-1", NULL);
    pkg_add_dep(keepme, "dep");
    repo_add(r, keepme); repo_add(r, dep);

    encoder *e = enc_new(r);
    /* Hard require only: no requests at all. */
    check("closure: require accepted", enc_require(e, "keepme", DEP_ANY, NULL));
    check("closure: require-only solve optimal", enc_solve(e) == MAXSAT_OPTIMAL);
    check("closure: required pkg chosen", enc_chosen_pkg(e, keepme));
    check("closure: its dependency chosen too", enc_chosen_pkg(e, dep));
    enc_free(e); repo_free(r);
}

int main(void) {
    printf("== Planet encoding tests ==\n");
    test_simple_dep();
    test_newest_wins();
    test_conflict();
    test_uninstallable();
    test_newest_feasible();
    test_provides();
    test_replaces();
    test_missing_request();
    test_closure_excludes_unreferenced();
    test_closure_keeps_required();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
