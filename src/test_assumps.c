/* Verify assumption-based solving + UNSAT-core extraction.
 *
 * The core-guided MaxSAT layer depends entirely on this being correct: it asks
 * "is the formula still satisfiable if I turn ON this set of soft selectors?"
 * and, when the answer is no, needs the exact subset of selectors responsible.
 */
#include "sat.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int cond) {
    printf("%s  %s\n", cond ? "PASS" : "FAIL", name);
    cond ? g_pass++ : g_fail++;
}

int main(void) {
    /* ---- core of {~a | b} with hard unit ~b ------------------------------
     * Hard: ~b. Soft-selector test: assume a AND b. Since hard forces ~b,
     * assuming b conflicts. The core must contain b (the culprit) and the
     * solver must report UNSAT. */
    {
        solver *s = sat_new();
        var a = sat_add_var(s), b = sat_add_var(s);
        lit nb = lit_mk(b, 1);
        sat_add_clause(s, &nb, 1);                    /* hard: ~b */
        lit disj[2] = { lit_mk(a,1), lit_mk(b,0) };    /* ~a | b  */
        sat_add_clause(s, disj, 2);

        lit assumps[2] = { lit_mk(a,0), lit_mk(b,0) }; /* assume a, b */
        lit core[4]; int cn = 0;
        int r = sat_solve_assumptions(s, assumps, 2, core, &cn);
        check("core: {a,b} under hard ~b -> UNSAT", r == 0);
        int has_b = 0;
        for (int i = 0; i < cn; i++) if (core[i] == lit_mk(b,0)) has_b = 1;
        check("core: contains b (the culprit)", has_b);

        /* now assume only a -> STILL UNSAT, and it's a great core check:
         * hard ~b plus (~a|b) means a implies b, which contradicts ~b.
         * So {a} alone is a minimal unsatisfiable core. */
        lit only_a[1] = { lit_mk(a,0) };
        lit core2[4]; int cn2 = 0;
        r = sat_solve_assumptions(s, only_a, 1, core2, &cn2);
        check("core: {a} alone -> UNSAT (a implies b, contradicts ~b)", r == 0);
        check("core: minimal core {a} has size 1", cn2 == 1 && core2[0] == lit_mk(a,0));
        sat_free(s);
    }

    /* ---- core picks the right subset of three ---------------------------
     * Hard: (~x | ~y). Assume x, y, z. x&y conflict; z is irrelevant.
     * Core must be {x,y} and must NOT include z. */
    {
        solver *s = sat_new();
        var x = sat_add_var(s), y = sat_add_var(s), z = sat_add_var(s);
        lit nxny[2] = { lit_mk(x,1), lit_mk(y,1) };    /* ~x | ~y */
        sat_add_clause(s, nxny, 2);

        lit assumps[3] = { lit_mk(x,0), lit_mk(y,0), lit_mk(z,0) };
        lit core[4]; int cn = 0;
        int r = sat_solve_assumptions(s, assumps, 3, core, &cn);
        check("core: {x,y,z} under ~x|~y -> UNSAT", r == 0);
        int has_x=0, has_y=0, has_z=0;
        for (int i = 0; i < cn; i++) {
            if (core[i]==lit_mk(x,0)) has_x=1;
            if (core[i]==lit_mk(y,0)) has_y=1;
            if (core[i]==lit_mk(z,0)) has_z=1;
        }
        check("core: contains x", has_x);
        check("core: contains y", has_y);
        check("core: excludes irrelevant z", !has_z);
        sat_free(s);
    }

    /* ---- reusable solver across many assumption sets --------------------
     * This is exactly how the MaxSAT loop drives it: same hard DB, vary the
     * assumption set, confirm SAT/UNSAT flips correctly each time. */
    {
        solver *s = sat_new();
        var p = sat_add_var(s), q = sat_add_var(s);
        lit cl[2] = { lit_mk(p,1), lit_mk(q,1) };      /* ~p | ~q */
        sat_add_clause(s, cl, 2);

        lit both[2] = { lit_mk(p,0), lit_mk(q,0) };
        lit just_p[1] = { lit_mk(p,0) };
        lit just_q[1] = { lit_mk(q,0) };
        check("reuse: {p,q} -> UNSAT", sat_solve_assumptions(s, both, 2, NULL, NULL) == 0);
        check("reuse: {p}   -> SAT",  sat_solve_assumptions(s, just_p, 1, NULL, NULL) == 1);
        check("reuse: {q}   -> SAT",  sat_solve_assumptions(s, just_q, 1, NULL, NULL) == 1);
        check("reuse: {p,q} -> UNSAT again", sat_solve_assumptions(s, both, 2, NULL, NULL) == 0);
        /* plain solve (no assumptions) is SAT: ~p|~q has models */
        check("reuse: no assumptions -> SAT", sat_solve(s) == 1);
        sat_free(s);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
