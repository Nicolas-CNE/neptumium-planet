/*
 * test_engine.c — differential test of the SAT engine UNDER ASSUMPTIONS.
 * ======================================================================
 *
 * The MaxSAT core-guided loop repeatedly drives the SAME solver with different
 * assumption sets, relying on (a) correct SAT/UNSAT answers and (b) learned
 * clauses staying SOUND across assumption changes (they must be implied by the
 * HARD clauses alone, never "borrowing" an assumption). If either breaks, the
 * MaxSAT costs go wrong. This test isolates the engine from the MaxSAT layer:
 *
 *   - build random hard clauses
 *   - reuse ONE solver for many random assumption sets
 *   - after EVERY solve, cross-check the SAT/UNSAT answer against brute force
 *
 * A single mismatch here means the engine (not the reweighting) is at fault.
 */
#include "sat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}

static unsigned long rng_state = 0x2545F4914F6CDD1DUL;
static unsigned rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 32);
}
static int rng_range(int lo, int hi) { return lo + (int)(rng_next() % (unsigned)(hi - lo + 1)); }

static int lit_true(lit l, unsigned mask) {
    int v = lit_var(l);
    int val = (mask >> v) & 1u;
    return lit_sign(l) ? !val : val;
}
static int clause_true(const lit *lits, int n, unsigned mask) {
    for (int i = 0; i < n; i++) if (lit_true(lits[i], mask)) return 1;
    return 0;
}

int main(void) {
    printf("== engine-under-assumptions differential tests ==\n");
    int total = 0, bad = 0, first_bad_ctx = -1;

    for (int it = 0; it < 2000; it++) {
        int nvars = rng_range(3, 10);
        int nh    = rng_range(1, 8);

        /* random hard clauses (vars 0..nvars-1) */
        lit *hlits[16]; int hn[16];
        for (int i = 0; i < nh; i++) {
            int n = rng_range(1, 3);
            hn[i] = n;
            hlits[i] = malloc((size_t)n * sizeof(lit));
            for (int k = 0; k < n; k++) {
                int v = rng_range(0, nvars - 1);
                hlits[i][k] = lit_mk(v, rng_range(0, 1));
            }
        }

        solver *s = sat_new();
        for (int v = 0; v < nvars; v++) sat_add_var(s);
        for (int i = 0; i < nh; i++) sat_add_clause(s, hlits[i], hn[i]);

        /* a handful of random assumption sets against the SAME solver */
        for (int rep = 0; rep < 8; rep++) {
            int na = rng_range(0, 4);
            lit assumps[4];
            for (int k = 0; k < na; k++) {
                int v = rng_range(0, nvars - 1);
                assumps[k] = lit_mk(v, rng_range(0, 1));
            }
            /* skip self-contradictory assumption sets (same var both polarities),
             * which are trivially UNSAT and fine but add no signal */
            int contra = 0;
            for (int a = 0; a < na && !contra; a++)
                for (int b = a + 1; b < na; b++)
                    if (lit_var(assumps[a]) == lit_var(assumps[b]) &&
                        lit_sign(assumps[a]) != lit_sign(assumps[b])) contra = 1;
            if (contra) continue;

            int got = sat_solve_assumptions(s, assumps, na, NULL, NULL);

            /* brute force: is there an assignment satisfying hard AND assumps? */
            int want_sat = 0;
            unsigned limit = 1u << nvars;
            for (unsigned mask = 0; mask < limit && !want_sat; mask++) {
                int ok = 1;
                for (int i = 0; i < nh && ok; i++)
                    if (!clause_true(hlits[i], hn[i], mask)) ok = 0;
                for (int k = 0; k < na && ok; k++)
                    if (!lit_true(assumps[k], mask)) ok = 0;
                if (ok) want_sat = 1;
            }
            int want = want_sat ? 1 : 0;
            total++;
            if (got != want) {
                bad++;
                if (first_bad_ctx < 0) {
                    first_bad_ctx = it;
                    fprintf(stderr, "[MISMATCH] solver=%s brute=%s (nvars=%d,na=%d)\n",
                            got ? "SAT" : "UNSAT", want ? "SAT" : "UNSAT", nvars, na);
                    fprintf(stderr, "  HARD (%d):\n", nh);
                    for (int i = 0; i < nh; i++) {
                        fprintf(stderr, "   ");
                        for (int k = 0; k < hn[i]; k++)
                            fprintf(stderr, " v%d%s", lit_var(hlits[i][k]),
                                    lit_sign(hlits[i][k]) ? "-" : "+");
                        fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "  ASSUMPTIONS (%d):", na);
                    for (int k = 0; k < na; k++)
                        fprintf(stderr, " v%d%s", lit_var(assumps[k]),
                                lit_sign(assumps[k]) ? "-" : "+");
                    fprintf(stderr, "\n");
                    if (want_sat) {
                        for (unsigned mask = 0; mask < limit; mask++) {
                            int ok2 = 1;
                            for (int i = 0; i < nh && ok2; i++)
                                if (!clause_true(hlits[i], hn[i], mask)) ok2 = 0;
                            for (int k = 0; k < na && ok2; k++)
                                if (!lit_true(assumps[k], mask)) ok2 = 0;
                            if (ok2) {
                                fprintf(stderr, "  BRUTE MODEL mask=0x%x:", mask);
                                for (int v = 0; v < nvars; v++)
                                    fprintf(stderr, " %d=%d", v, (mask >> v) & 1u);
                                fprintf(stderr, "\n");
                                break;
                            }
                        }
                    }
                }
            }
        }

        sat_free(s);
        for (int i = 0; i < nh; i++) free(hlits[i]);
    }

    char name[128];
    snprintf(name, sizeof name, "engine: %d assumption-solves match brute force", total);
    check(name, bad == 0);
    if (bad) printf("       -> %d mismatches, first near iter %d\n", bad, first_bad_ctx);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
