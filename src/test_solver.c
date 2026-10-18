/* planet test harness.
 * Exercises the CDCL engine and AMO encodings on known-SAT and known-UNSAT
 * instances, then (if given a file) solves a DIMACS CNF. */
#include "sat.h"
#include "amo.h"
#include "dimacs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void check(const char *name, int got, int want) {
    if (got == want) { printf("PASS  %-34s -> %s\n", name, got ? "SAT" : "UNSAT"); g_pass++; }
    else             { printf("FAIL  %-34s -> %s (want %s)\n", name,
                              got ? "SAT" : "UNSAT", want ? "SAT" : "UNSAT"); g_fail++; }
}

/* --- tiny hand-built cases --- */

static void test_trivial_sat(void) {
    solver *s = sat_new();
    var x = sat_add_var(s);
    lit c1[] = { lit_mk(x, 0) };
    lit c2[] = { lit_mk(x, 1) };
    sat_add_clause(s, c1, 1);
    sat_add_clause(s, c2, 1);
    check("trivial x / ~x", sat_solve(s), 0);   /* x and ~x -> UNSAT */
    sat_free(s);
}

static void test_unit_chain_sat(void) {
    solver *s = sat_new();
    var a = sat_add_var(s), b = sat_add_var(s);
    lit c1[] = { lit_mk(a, 0) };                 /* a */
    lit c2[] = { lit_mk(a, 1), lit_mk(b, 0) };   /* ~a | b */
    sat_add_clause(s, c1, 1);
    sat_add_clause(s, c2, 2);
    check("unit chain a, ~a|b", sat_solve(s), 1);
    if (sat_val_var(s, b) != VAL_TRUE) { printf("FAIL  unit chain propagated b\n"); g_fail--; g_fail++; }
    sat_free(s);
}

static void test_pigeonhole(void) {
    /* PHP(3,2): 3 pigeons, 2 holes -> UNSAT. Each pigeon in >=1 hole,
     * each hole holds <=1 pigeon (AMO over pigeons per hole). */
    int pigeons = 3, holes = 2;
    solver *s = sat_new();
    var **p = calloc((size_t)pigeons, sizeof *p);
    for (int i = 0; i < pigeons; i++) {
        p[i] = calloc((size_t)holes, sizeof *p[i]);
        for (int j = 0; j < holes; j++) p[i][j] = sat_add_var(s);
    }
    /* pigeon i occupies some hole */
    for (int i = 0; i < pigeons; i++) {
        lit *cls = malloc((size_t)holes * sizeof *cls);
        for (int j = 0; j < holes; j++) cls[j] = lit_mk(p[i][j], 0);
        sat_add_clause(s, cls, holes);
        free(cls);
    }
    /* each hole: at most one pigeon */
    for (int j = 0; j < holes; j++) {
        lit *xs = malloc((size_t)pigeons * sizeof *xs);
        for (int i = 0; i < pigeons; i++) xs[i] = lit_mk(p[i][j], 0);
        amo_sequential_ladder(s, xs, pigeons);
        free(xs);
    }
    check("pigeonhole PHP(3,2)", sat_solve(s), 0);
    for (int i = 0; i < pigeons; i++) free(p[i]);
    free(p);
    sat_free(s);
}

static void test_pigeonhole_sat(void) {
    /* PHP(2,2): 2 pigeons, 2 holes -> SAT. */
    int pigeons = 2, holes = 2;
    solver *s = sat_new();
    var **p = calloc((size_t)pigeons, sizeof *p);
    for (int i = 0; i < pigeons; i++) {
        p[i] = calloc((size_t)holes, sizeof *p[i]);
        for (int j = 0; j < holes; j++) p[i][j] = sat_add_var(s);
    }
    for (int i = 0; i < pigeons; i++) {
        lit *cls = malloc((size_t)holes * sizeof *cls);
        for (int j = 0; j < holes; j++) cls[j] = lit_mk(p[i][j], 0);
        sat_add_clause(s, cls, holes);
        free(cls);
    }
    for (int j = 0; j < holes; j++) {
        lit *xs = malloc((size_t)pigeons * sizeof *xs);
        for (int i = 0; i < pigeons; i++) xs[i] = lit_mk(p[i][j], 0);
        amo_sequential_ladder(s, xs, pigeons);
        free(xs);
    }
    check("pigeonhole PHP(2,2)", sat_solve(s), 1);
    for (int i = 0; i < pigeons; i++) free(p[i]);
    free(p);
    sat_free(s);
}

/* Cross-check: ladder vs pairwise AMO agree on a forced-2-true case. */
static void test_amo_forces_unsat(void) {
    for (int enc = 0; enc < 2; enc++) {
        solver *s = sat_new();
        int n = 5;
        lit *xs = malloc((size_t)n * sizeof *xs);
        for (int i = 0; i < n; i++) xs[i] = lit_mk(sat_add_var(s), 0);
        if (enc == 0) amo_sequential_ladder(s, xs, n);
        else          amo_pairwise(s, xs, n);
        /* force TWO literals true via unit clauses -> violates AMO -> UNSAT */
        lit u0 = xs[0], u1 = xs[1];
        sat_add_clause(s, &u0, 1);
        sat_add_clause(s, &u1, 1);
        check(enc == 0 ? "AMO(ladder) force2" : "AMO(pairwise) force2", sat_solve(s), 0);
        free(xs);
        sat_free(s);
    }
}

/* Random 3-SAT sanity: build a satisfiable instance (plant a solution). */
static void test_random3sat_planted(void) {
    solver *s = sat_new();
    int n = 60, m = 200;
    var *vars = malloc((size_t)n * sizeof *vars);
    for (int i = 0; i < n; i++) vars[i] = sat_add_var(s);
    /* planted solution: first half true */
    int *sol = calloc((size_t)n, sizeof *sol);
    for (int i = 0; i < n; i++) sol[i] = (i < n/2);
    srand(12345);
    for (int k = 0; k < m; k++) {
        lit cls[3];
        for (int t = 0; t < 3; t++) {
            int vi = rand() % n;
            int want = sol[vi];
            /* pick polarity so planted sol satisfies this literal */
            cls[t] = lit_mk(vars[vi], want ? 0 : 1);
        }
        sat_add_clause(s, cls, 3);
    }
    check("planted 3-SAT (n=60,m=200)", sat_solve(s), 1);
    free(sol); free(vars);
    sat_free(s);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        /* solve a DIMACS file */
        solver *s = sat_new();
        FILE *fp = fopen(argv[1], "r");
        if (!fp) { perror(argv[1]); return 2; }
        if (!dimacs_read(s, fp)) { fprintf(stderr, "parse error\n"); return 2; }
        fclose(fp);
        int r = sat_solve(s);
        printf("%s\n", r ? "SAT" : "UNSAT");
        if (r) {
            for (var v = 0; v < sat_nvars(s); v++)
                printf("%s%d ", sat_val_var(s, v) == VAL_TRUE ? "" : "-", v + 1);
            printf("0\n");
        }
        sat_stats st; sat_stats_get(s, &st);
        if (argc > 2 && strcmp(argv[2], "-S") == 0) sat_print_stats(stdout, &st);
        sat_free(s);
        return r ? 10 : 20;
    }

    printf("== planet solver core tests ==\n");
    test_trivial_sat();
    test_unit_chain_sat();
    test_pigeonhole();
    test_pigeonhole_sat();
    test_amo_forces_unsat();
    test_random3sat_planted();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
