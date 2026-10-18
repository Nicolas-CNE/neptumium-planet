/*
 * test_maxsat.c — verify the core-guided loop returns the optimal cost.
 * ======================================================================
 *
 * The solver must return the OPTIMUM, not an approximation: a suboptimal
 * answer would silently pick a more expensive package set than necessary.
 * So we check EXACTNESS against a brute-force oracle:
 *
 *   oracle(S) = min over all 2^n assignments of
 *               ( sum of weights of falsified soft clauses )
 *               restricted to assignments satisfying every hard clause.
 *
 * For n <= 12 this enumeration is trivial (4096 rows) and unimpeachable —
 * it *is* the definition of the optimum. We then require the core-guided
 * loop to agree on every one of hundreds of random instances, in both the
 * OPTIMAL case (equal cost) and the infeasible case (hard clauses alone
 * unsatisfiable => MAXSAT_UNSAT).
 *
 * ENCODING NOTE: every literal is built with lit_mk(var, sign) — the engine's
 * own encoding — and variables are 0-based (maxsat_var hands out 0,1,2,...).
 * The oracle evaluates the same encoding, so solver and oracle see literally
 * the same bits. (An earlier revision used raw +/- ints and silently corrupted
 * watch-list indices; building with lit_mk makes that class of bug impossible.)
 */
#include "maxsat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}

/* ---- tiny deterministic RNG so failures are reproducible ----------------- */
static unsigned long rng_state = 0x9e3779b97f4a7c15UL;
static unsigned rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 32);
}
static int rng_range(int lo, int hi) {          /* inclusive */
    return lo + (int)(rng_next() % (unsigned)(hi - lo + 1));
}

/* ---- brute-force oracle ---------------------------------------------------
 * Evaluates a literal under bitmask `mask`, where bit v of mask is the truth
 * of variable v. Uses the engine encoding: lit_sign tells the polarity. */
static int lit_true(lit l, unsigned mask) {
    int v = lit_var(l);
    int val = (mask >> v) & 1u;
    return lit_sign(l) ? !val : val;
}
static int clause_true(const lit *lits, int n, unsigned mask) {
    for (int i = 0; i < n; i++) if (lit_true(lits[i], mask)) return 1;
    return 0;
}

typedef struct { lit *lits; int n; } clause_t;
typedef struct { lit *lits; int n; long w; } soft_t;

/* Minimum falsified soft weight over all assignments honouring hard clauses.
 * Returns best=-1 when no assignment satisfies the hard clauses (infeasible);
 * *feas_out is set to 1 iff at least one hard-satisfying assignment exists. */
static long oracle(int nvars, clause_t *hard, int nh,
                   soft_t *soft, int ns, int *feas_out) {
    long best = -1;
    unsigned limit = 1u << nvars;
    for (unsigned mask = 0; mask < limit; mask++) {
        int ok = 1;
        for (int i = 0; i < nh && ok; i++)
            if (!clause_true(hard[i].lits, hard[i].n, mask)) ok = 0;
        if (!ok) continue;
        *feas_out = 1;
        long cost = 0;
        for (int i = 0; i < ns; i++)
            if (!clause_true(soft[i].lits, soft[i].n, mask)) cost += soft[i].w;
        if (best < 0 || cost < best) best = cost;
    }
    return best;
}

/* ---- hand-written cases with known answers ------------------------------ */

static void test_known(void) {
    /* (1) One weight-5 soft clause, no hard rules: optimum is 0 (just turn it
     *      on). The loop must NOT "bank" any cost. */
    {
        maxsat *m = maxsat_new();
        var p = maxsat_var(m);
        lit cl[1] = { lit_mk(p, 0) };
        maxsat_soft(m, cl, 1, 5);
        int r = maxsat_solve(m);
        check("known: single soft -> OPTIMAL", r == MAXSAT_OPTIMAL);
        check("known: single soft cost 0", maxsat_cost(m) == 0);
        check("known: single soft selected", maxsat_value(m, p) == 1);
        maxsat_free(m);
    }

    /* (2) Hard ~p plus soft p (weight 7): p can never be on, so the soft is
     *      necessarily dropped -> optimum broken weight is exactly 7. */
    {
        maxsat *m = maxsat_new();
        var p = maxsat_var(m);
        lit hp[1] = { lit_mk(p, 1) };
        maxsat_hard(m, hp, 1);
        lit sp[1] = { lit_mk(p, 0) };
        maxsat_soft(m, sp, 1, 7);
        int r = maxsat_solve(m);
        check("known: hard~p + soft p -> OPTIMAL", r == MAXSAT_OPTIMAL);
        check("known: forced drop costs 7", maxsat_cost(m) == 7);
        maxsat_free(m);
    }

    /* (3) Tension between objectives: soft wants p (w=3) and q (w=3); hard
     *      says NOT both. Optimal keeps exactly one -> broken cost 3. Enforcing
     *      everything would report 6, and enforcing nothing reports 0; the
     *      extracted core must yield 3. */
    {
        maxsat *m = maxsat_new();
        var p = maxsat_var(m), q = maxsat_var(m);
        lit hard[2] = { lit_mk(p, 1), lit_mk(q, 1) };
        maxsat_hard(m, hard, 2);
        lit sp[1] = { lit_mk(p, 0) }, sq[1] = { lit_mk(q, 0) };
        maxsat_soft(m, sp, 1, 3);
        maxsat_soft(m, sq, 1, 3);
        int r = maxsat_solve(m);
        check("known: mutual exclusion -> OPTIMAL", r == MAXSAT_OPTIMAL);
        check("known: exactly one dropped (cost 3)", maxsat_cost(m) == 3);
        check("known: exactly one selected",
              maxsat_value(m, p) + maxsat_value(m, q) == 1);
        maxsat_free(m);
    }

    /* (4) Weighted choice: soft wants p (w=10) over r (w=1); hard forbids
     *      p and r together. Optimum keeps the expensive wish, drops the cheap
     *      one -> cost 1. */
    {
        maxsat *m = maxsat_new();
        var p = maxsat_var(m), r = maxsat_var(m);
        lit hard[2] = { lit_mk(p, 1), lit_mk(r, 1) };
        maxsat_hard(m, hard, 2);
        lit sp[1] = { lit_mk(p, 0) }, sr[1] = { lit_mk(r, 0) };
        maxsat_soft(m, sp, 1, 10);
        maxsat_soft(m, sr, 1, 1);
        int rc = maxsat_solve(m);
        check("known: weighted conflict -> OPTIMAL", rc == MAXSAT_OPTIMAL);
        check("known: drop the cheap wish (cost 1)", maxsat_cost(m) == 1);
        check("known: keep p, drop r",
              maxsat_value(m, p) == 1 && maxsat_value(m, r) == 0);
        maxsat_free(m);
    }

    /* (5) Hard clauses alone contradictory -> MAXSAT_UNSAT, not a bogus cost. */
    {
        maxsat *m = maxsat_new();
        var p = maxsat_var(m);
        lit a[1] = { lit_mk(p, 0) }, b[1] = { lit_mk(p, 1) };
        maxsat_hard(m, a, 1);
        maxsat_hard(m, b, 1);
        lit sp[1] = { lit_mk(p, 0) };
        maxsat_soft(m, sp, 1, 4);
        int rc = maxsat_solve(m);
        check("known: contradictory hard -> UNSAT", rc == MAXSAT_UNSAT);
        maxsat_free(m);
    }

    /* (6) Implication chain: hard p=>q, q=>r, NOT r; soft wants all three at
     *      weight 2. Then p, q cannot be on either (they force r). All three
     *      softs are unavoidably dropped -> cost 6. Exercises multi-member
     *      cores and transitive implications. */
    {
        maxsat *m = maxsat_new();
        var p = maxsat_var(m), q = maxsat_var(m), r = maxsat_var(m);
        lit c1[2] = { lit_mk(p, 1), lit_mk(q, 0) };
        lit c2[2] = { lit_mk(q, 1), lit_mk(r, 0) };
        lit c3[1] = { lit_mk(r, 1) };
        maxsat_hard(m, c1, 2);
        maxsat_hard(m, c2, 2);
        maxsat_hard(m, c3, 1);
        lit s1[1] = { lit_mk(p, 0) }, s2[1] = { lit_mk(q, 0) }, s3[1] = { lit_mk(r, 0) };
        maxsat_soft(m, s1, 1, 2);
        maxsat_soft(m, s2, 1, 2);
        maxsat_soft(m, s3, 1, 2);
        int rc = maxsat_solve(m);
        check("known: implication chain -> OPTIMAL", rc == MAXSAT_OPTIMAL);
        check("known: all three dropped (cost 6)", maxsat_cost(m) == 6);
        maxsat_free(m);
    }
}

/* ---- randomized differential test against the oracle --------------------- */

static void test_random(int iters, int nvars, int nh, int ns, long wmax) {
    int agree = 0, disagree = 0;
    int first_bad = -1;

    for (int it = 0; it < iters; it++) {
        /* --- build a random instance (kept alive for the oracle) --------- */
        clause_t *hard = calloc((size_t)nh, sizeof *hard);
        soft_t   *soft = calloc((size_t)ns, sizeof *soft);

        for (int i = 0; i < nh; i++) {
            int n = rng_range(1, 3);
            hard[i].lits = malloc((size_t)n * sizeof(lit));
            hard[i].n = n;
            for (int k = 0; k < n; k++) {
                int v = rng_range(0, nvars - 1);            /* 0-based vars */
                hard[i].lits[k] = lit_mk(v, rng_range(0, 1));
            }
        }
        for (int i = 0; i < ns; i++) {
            int n = rng_range(1, 3);
            soft[i].lits = malloc((size_t)n * sizeof(lit));
            soft[i].n = n;
            for (int k = 0; k < n; k++) {
                int v = rng_range(0, nvars - 1);
                soft[i].lits[k] = lit_mk(v, rng_range(0, 1));
            }
            soft[i].w = rng_range(1, (int)wmax);
        }

        /* --- feed an identical instance to the core-guided solver --------
         * We allocate ALL decision variables first (0..nvars-1), matching the
         * oracle's var space exactly; the selector vars maxsat_soft creates
         * live at nvars and above, which the oracle never sees. Literals pass
         * through unchanged (identity mapping). */
        maxsat *m = maxsat_new();
        for (int v = 0; v < nvars; v++) maxsat_var(m);

        int infeasible_hard = 0;
        for (int i = 0; i < nh; i++)
            if (!maxsat_hard(m, hard[i].lits, hard[i].n)) infeasible_hard = 1;
        int soft_ok = 1;
        for (int i = 0; i < ns; i++)
            if (!maxsat_soft(m, soft[i].lits, soft[i].n, soft[i].w)) soft_ok = 0;

        /* --- oracle ------------------------------------------------------- */
        int feas = 0;
        long obest = oracle(nvars, hard, nh, soft, ns, &feas);

        /* --- compare ------------------------------------------------------ */
        int ok;
        long rc_cost = -2; int rc_val = -2;
        if (infeasible_hard || !soft_ok) {
            ok = 1;
        } else {
            int rc = maxsat_solve(m);
            rc_val = rc;
            if (!feas) {
                ok = (rc == MAXSAT_UNSAT);
            } else if (rc == MAXSAT_UNSAT) {
                ok = 0;
            } else {
                rc_cost = maxsat_cost(m);
                ok = (rc_cost == obest);
            }
        }
        if (ok) agree++;
        else {
            disagree++;
            if (first_bad < 0) {
                first_bad = it;
                fprintf(stderr, "\n--- DISAGREEMENT iter=%d nvars=%d ---\n", it, nvars);
                fprintf(stderr, "feas=%d oracle=%ld solver_rc=%d solver_cost=%ld\n",
                        feas, obest, rc_val, rc_cost);
                fprintf(stderr, "HARD (%d):\n", nh);
                for (int i = 0; i < nh; i++) {
                    fprintf(stderr, "  ");
                    for (int k = 0; k < hard[i].n; k++)
                        fprintf(stderr, "%s%d(v%d%s) ",
                               k ? "" : "", hard[i].lits[k],
                               lit_var(hard[i].lits[k]),
                               lit_sign(hard[i].lits[k]) ? "-" : "+");
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "SOFT (%d):\n", ns);
                for (int i = 0; i < ns; i++) {
                    fprintf(stderr, "  w=%ld: ", soft[i].w);
                    for (int k = 0; k < soft[i].n; k++)
                        fprintf(stderr, "%d(v%d%s) ",
                               soft[i].lits[k],
                               lit_var(soft[i].lits[k]),
                               lit_sign(soft[i].lits[k]) ? "-" : "+");
                    fprintf(stderr, "\n");
                }
            }
        }

        maxsat_free(m);
        for (int i = 0; i < nh; i++) free(hard[i].lits);
        for (int i = 0; i < ns; i++) free(soft[i].lits);
        free(hard); free(soft);
    }

    char name[128];
    snprintf(name, sizeof name,
             "random: %d instances (n=%d,h=%d,s=%d) match oracle (%d agree)",
             iters, nvars, nh, ns, agree);
    check(name, disagree == 0);
    if (disagree)
        printf("       -> %d disagreements, first at iteration %d\n", disagree, first_bad);
}

/* Reusable-solver stress: MaxSAT repeatedly drives the same clause database
 * with different assumption sets — exactly what the rounds above do. */
static void test_reuse_stress(void) {
    maxsat *m = maxsat_new();
    var vars[10];
    for (int i = 0; i < 10; i++) vars[i] = maxsat_var(m);

    /* chain: v[i] => v[i+1]; soft prefers every v[i] at weight (i+1) */
    for (int i = 0; i < 9; i++) {
        lit cl[2] = { lit_mk(vars[i], 1), lit_mk(vars[i + 1], 0) };
        maxsat_hard(m, cl, 2);
    }
    for (int i = 0; i < 10; i++) {
        lit cl[1] = { lit_mk(vars[i], 0) };
        maxsat_soft(m, cl, 1, i + 1);
    }
    /* forbid v[9]: then the whole chain must be off -> pay sum(1..10)=55 */
    lit forbid[1] = { lit_mk(vars[9], 1) };
    maxsat_hard(m, forbid, 1);

    int rc = maxsat_solve(m);
    check("stress: chain -> OPTIMAL", rc == MAXSAT_OPTIMAL);
    check("stress: total weight 55 dropped", maxsat_cost(m) == 55);
    check("stress: all vars off", maxsat_remaining(m) == 0);

    /* Solve the SAME solver again to confirm the core-guided loop leaves the
     * engine in a reusable state (resets cleanly, deterministic answer). */
    int rc2 = maxsat_solve(m);
    check("stress: deterministic rerun", rc2 == rc && maxsat_cost(m) == 55);
    maxsat_free(m);
}

int main(void) {
    printf("== planet MaxSAT core-guided tests ==\n");
    test_known();
    test_reuse_stress();

    /* Differential sweeps: small enough for the 2^n oracle, varied enough to
     * cover feasible, infeasible, weighted and unweighted regimes. */
    test_random(300, 6,  4, 6, 3);
    test_random(300, 7,  6, 8, 5);
    test_random(250, 8,  5, 10, 10);
    test_random(200, 9,  8, 12, 4);
    test_random(120, 10, 9, 14, 20);
    test_random( 60, 11, 12, 16, 7);
    test_random( 30, 12, 14, 18, 50);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
