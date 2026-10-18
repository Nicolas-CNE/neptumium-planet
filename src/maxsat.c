/*
 * maxsat.c — weighted partial MaxSAT via the Implicit Hitting Set (IHS)
 *            (a.k.a. "core-guided by hitting set" / MaxSAT-by-IHS) algorithm.
 * ======================================================================
 *
 * WHY IHS (AND NOT THE NAIVE "REWEIGHTING" LOOP)
 * ----------------------------------------------
 * The obvious core-guided loop — "assume every soft on; on UNSAT extract a
 * core, bank min-weight, subtract it from every core member, repeat" — is
 * *not* a sound exact algorithm. Its answer depends on WHICH minimal core the
 * extractor happens to return, and it can under-pay. We hit exactly this on a
 * 7-variable instance (test_maxsat): the core {v4+, v2-} banked 2, but the
 * greedy "enforce everything still positive-weight" then forced BOTH of those
 * softs off (real cost 4) while the true optimum (drop the cheap v2+ plus v4+)
 * cost 3. The reported `paid` was neither the model's cost nor the optimum.
 * A different valid core for the same instance gave the right answer — proof
 * the method is a heuristic, not an exact solver. So we use IHS, which is
 * optimal for ANY sequence of (sound) cores.
 *
 * THE ALGORITHM
 * -------------
 * View MaxSAT as: choose a set D of soft clauses to DROP (pay their weight)
 * so that  hard ^ { soft_i : i not in D }  is satisfiable, minimising
 * sum_{i in D} w_i.
 *
 *   Core:   a set C of softs that cannot all be enforced (hard ^ all of C is
 *           UNSAT). Every feasible D must DROP at least one member of C
 *           (hit C). Cores are sound, minimal, and found by proof.
 *
 *   Master: given the cores found so far, pick the minimum-weight set D
 *           that hits every one of them (a weighted hitting set, solved
 *           exactly by the branch-and-bound below)
 *
 *   Oracle: try to enforce the complement of D (everything not dropped). If
 *           satisfiable, D is feasible. Because D is the cheapest set hitting
 *           every known core, and the true optimum must also hit every core,
 *           cost(D) <= cost(opt) <= cost(D) — so D is OPTIMAL. If UNSAT, the
 *           oracle returns a NEW core C subset of the enforced softs; add it
 *           and repeat. Each new core forbids the current D (D misses C), so
 *           the loop makes progress and terminates.
 *
 * CORRECTNESS SKETCH: every returned core is a genuine "cannot all be
 * enforced" set, so the true optimum's dropped set hits all of them; the
 * master's D is therefore a LOWER bound on the optimum. When the oracle
 * confirms D itself is feasible, that lower bound is achieved, proving
 * optimality. This holds regardless of which minimal cores the oracle yields,
 * which is exactly the property the reweighting loop lacked.
 *
 * CORE EXTRACTION — DEletion FILTER (BY PROOF, NOT HEURISTICS)
 * -----------------------------------------------------------
 * The oracle's cores must be genuinely UNSAT (hard ^ all-core-selectors-ON).
 * A cheap in-conflict "final analysis" heuristic can under-report the
 * responsible assumptions; if it ever omits one, the derived constraint would
 * wrongly exclude valid package sets. So we extract cores by PROOF: the
 * deletion filter re-solves to confirm each assumption is necessary, yielding
 * a *minimal* unsatisfiable subset — sound because every shrink step was
 * verified UNSAT by a real solve.
 *
 * LITERAL ENCODING: all clause literals are the engine's `lit` (lit_mk):
 * non-negative, bit0 = sign, rest = variable. Selectors are stored as `var`
 * and converted with lit_mk at each use site. This matches sat.c exactly, so
 * watch-list indexing (which uses the literal value directly) is always in
 * range.
 *
 * OBJECTIVE NOTE: we minimise *falsified soft weight*. In Planet's encoding
 * a "prefer package" soft clause is positive (dropping it costs weight) and
 * "avoid package" is expressed by giving the *absence* the soft clause, so
 * the minimisation reads naturally as "break as few user wishes as possible".
 */
#include "maxsat.h"
#include "sat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- optional round-by-round trace (set NEPT_MAXSAT_DEBUG=1) ------------- */
static int mdbg(void) {
    static int d = -1;
    if (d < 0) d = getenv("NEPT_MAXSAT_DEBUG") ? 1 : 0;
    return d;
}

/* One soft clause as tracked by IHS. */
typedef struct {
    var   sel;          /* selector variable (engine var index) */
    long  orig;         /* original weight (= cost of dropping it) */
} soft_clause;

struct maxsat {
    solver *s;             /* the CDCL engine doing the actual search */

    /* Hard clauses are simply pushed into `s` as they arrive. */

    soft_clause *softs;    /* all soft clauses ever added */
    int   nsofts, cap_softs;

    /* Accumulated cores from the oracle. Each core is a set of soft INDICES
     * that cannot all be enforced; the master must hit every one. Cores are
     * stored as OFFSETS into core_pool (never raw pointers), because core_pool
     * is realloc'd as it grows — a stored pointer would dangle. */
    int  *core_pool;       /* flat storage of all core members */
    int  *core_len;        /* core_len[c]  = size of core c */
    int  *core_off;        /* core_off[c]  = start index into core_pool */
    int   ncores, cap_cores;
    int   pool_len, cap_pool;

    long  best_cost;       /* valid after MAXSAT_OPTIMAL */
    int   nkept;           /* soft clauses satisfied (not dropped) in optimum */
    int   solved;
    long  rounds;          /* IHS iterations executed (reported by -S) */

    /* Scratch buffer reused each round to hold the assumption vector. */
    lit  *assumps;
    int   cap_assumps;

    /* Scratch for the master B&B (allocated once, reused). */
    int  *bb_drop;         /* current dropped[] during B&B */
    int  *bb_best;         /* best dropped[] found so far */
    int  *bb_hit;          /* which cores are already hit */
};

static void mtrace(const maxsat *m, const char *fmt, ...) {
    if (!mdbg()) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[maxsat] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "   (rounds=%ld cores=%d)\n", m->rounds, m->ncores);
}

/* ---- lifecycle ---------------------------------------------------------- */

maxsat *maxsat_new(void) {
    maxsat *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->s = sat_new();
    if (!m->s) { free(m); return NULL; }
    return m;
}

void maxsat_free(maxsat *m) {
    if (!m) return;
    free(m->softs);
    free(m->core_len);
    free(m->core_off);
    free(m->core_pool);
    free(m->assumps);
    free(m->bb_drop);
    free(m->bb_best);
    free(m->bb_hit);
    sat_free(m->s);
    free(m);
}

var maxsat_var(maxsat *m)        { return sat_add_var(m->s); }
int maxsat_nvars(const maxsat *m){ return sat_nvars(m->s); }
solver *maxsat_engine(maxsat *m) { return m ? m->s : NULL; }

/* ---- clause registration ------------------------------------------------ */

int maxsat_hard(maxsat *m, const lit *lits, int n) {
    /* The engine copies the literal array, so dropping const here is safe. */
    return sat_add_clause(m->s, (lit *)lits, n);
}

int maxsat_soft(maxsat *m, const lit *lits, int n, long weight) {
    if (n < 1 || weight <= 0) return 0;

    /* The guarded form is (~sel v l_1 v ... v l_n): if the selector sel is ON,
     * the ~sel literal is false so the original clause must hold; if the
     * selector is OFF, ~sel is true and the guard trivially satisfies the
     * clause — that's when we pay the weight. */
    var sel = sat_add_var(m->s);

    lit *buf = malloc((size_t)(n + 1) * sizeof *buf);
    if (!buf) return 0;
    buf[0] = lit_mk(sel, 1);                        /* ~sel */
    memcpy(buf + 1, lits, (size_t)n * sizeof *lits);
    int ok = sat_add_clause(m->s, buf, n + 1);
    free(buf);
    if (!ok) return 0;                              /* degenerate empty soft */

    if (m->nsofts == m->cap_softs) {
        m->cap_softs = m->cap_softs ? m->cap_softs * 2 : 64;
        soft_clause *p = realloc(m->softs, (size_t)m->cap_softs * sizeof *p);
        if (!p) return 0;
        m->softs = p;
    }
    m->softs[m->nsofts].sel  = sel;
    m->softs[m->nsofts].orig = weight;
    m->nsofts++;
    m->solved = 0;
    return 1;
}

/* ---- core storage ------------------------------------------------------- */

static int core_push(maxsat *m, const int *idx, int n) {
    if (n <= 0) return 0;
    if (m->ncores == m->cap_cores) {
        m->cap_cores = m->cap_cores ? m->cap_cores * 2 : 16;
        int  *cl = realloc(m->core_len, (size_t)m->cap_cores * sizeof *cl);
        int  *co = realloc(m->core_off, (size_t)m->cap_cores * sizeof *co);
        if (!cl || !co) { m->core_len = cl; m->core_off = co; return 0; }
        m->core_len = cl; m->core_off = co;
    }
    if (m->pool_len + n > m->cap_pool) {
        m->cap_pool = (m->pool_len + n) * 2;
        int *p = realloc(m->core_pool, (size_t)m->cap_pool * sizeof *p);
        if (!p) return 0;
        m->core_pool = p;
    }
    m->core_off[m->ncores] = m->pool_len;
    m->core_len[m->ncores] = n;
    memcpy(m->core_pool + m->pool_len, idx, (size_t)n * sizeof *idx);
    m->pool_len += n;
    m->ncores++;
    return 1;
}

/* ---- feasibility oracle + core extraction -------------------------------- */

/* Ask the engine whether (hard ^ these selectors ON) is UNSAT, without keeping
 * any model. Leaves the solver in a clean units-only state either way. */
static int subset_is_unsat(maxsat *m, const lit *assumps, int n) {
    int rc = sat_solve_assumptions(m->s, assumps, n, NULL, NULL);
    if (rc == 0) return 1;          /* UNSAT — engine already reset to clean */
    sat_reset(m->s);                /* SAT — drop the model, back to clean */
    return 0;
}

/* Enforce every soft whose index is in S[0..nS-1]. If satisfiable return -1.
 * Otherwise extract a MINIMAL core (subset of S, by the deletion filter) into
 * core_out and return its size. Leaves the solver clean either way. */
static int extract_core(maxsat *m, const int *S, int nS, int *core_out) {
    if (m->cap_assumps < nS) {
        m->cap_assumps = nS;
        lit *p = realloc(m->assumps, (size_t)m->cap_assumps * sizeof *p);
        if (!p) return -2;                      /* OOM: signal hard failure */
        m->assumps = p;
    }
    for (int i = 0; i < nS; i++)
        m->assumps[i] = lit_mk(m->softs[S[i]].sel, 0);   /* +sel (ON) */

    int rc = sat_solve_assumptions(m->s, m->assumps, nS, NULL, NULL);
    /* FEASIBLE: keep the model in the engine (do NOT reset) so that
     * maxsat_value() can read the optimal assignment after we return. The
     * caller returns OPTIMAL immediately on -1, so no further solve will
     * clobber this model. */
    if (rc == 1) return -1;

    /* UNSAT: deletion filter for a minimal core. */
    int *keep = malloc((size_t)nS * sizeof *keep);
    lit *sub  = malloc((size_t)nS * sizeof *sub);
    if (!keep || !sub) { free(keep); free(sub); return -2; }
    for (int i = 0; i < nS; i++) keep[i] = 1;

    for (int i = 0; i < nS; i++) {
        int ns = 0;
        for (int j = 0; j < nS; j++)
            if (keep[j] && j != i) sub[ns++] = lit_mk(m->softs[S[j]].sel, 0);
        if (subset_is_unsat(m, sub, ns)) keep[i] = 0;    /* i not required */
    }
    int nc = 0;
    for (int i = 0; i < nS; i++) if (keep[i]) core_out[nc++] = S[i];
    free(keep); free(sub);
    sat_reset(m->s);
    return nc;
}

/* ---- master: exact minimum-weight hitting set over the cores ------------- *
 * We must choose a set D of softs to drop, hitting every known core, with
 * minimum total orig weight. This is weighted hitting set; we solve it EXACTLY
 * by branch-and-bound: repeatedly pick an un-hit core and branch on which of
 * its members to include in D, pruning whenever the running cost meets or
 * exceeds the best complete solution found so far. The initial best is the
 * trivial "drop everything" solution, so the search is always bounded.       */

typedef struct {
    maxsat *m;
    long    best;
} bb_ctx;

static void ms_bb(bb_ctx *ctx, long cost, int *hit) {
    maxsat *m = ctx->m;
    if (cost >= ctx->best) return;                     /* prune */

    /* find the first un-hit core */
    int c = -1;
    for (int j = 0; j < m->ncores; j++) if (!hit[j]) { c = j; break; }
    if (c < 0) {                                       /* all cores hit */
        ctx->best = cost;
        memcpy(m->bb_best, m->bb_drop, (size_t)m->nsofts * sizeof *m->bb_drop);
        return;
    }

    int  len  = m->core_len[c];
    int  off  = m->core_off[c];
    /* per-level scratch: which cores this branch newly hit (must be LOCAL, not
     * shared, because ms_bb recurses — a shared array would be clobbered by
     * nested calls and the hit[] restore would go wrong). */
    int  mark[m->ncores];
    for (int k = 0; k < len; k++) {
        int i = m->core_pool[off + k];
        if (m->bb_drop[i]) continue;                   /* already dropped */
        m->bb_drop[i] = 1;
        /* mark every previously un-hit core that now contains i */
        int changed = 0;
        for (int j = 0; j < m->ncores; j++) {
            if (hit[j]) continue;
            int lj = m->core_len[j], oj = m->core_off[j];
            for (int t = 0; t < lj; t++) if (m->core_pool[oj + t] == i) { hit[j] = 1; mark[changed++] = j; break; }
        }
        ms_bb(ctx, cost + m->softs[i].orig, hit);
        for (int t = 0; t < changed; t++) hit[mark[t]] = 0;   /* unmark */
        m->bb_drop[i] = 0;
    }
}

/* Fill dropped[0..nsofts-1] with the min-weight hitting set; return its cost. */
static long master_hitting_set(maxsat *m, int *dropped) {
    long total = 0;
    for (int i = 0; i < m->nsofts; i++) total += m->softs[i].orig;

    if (m->ncores == 0) {                              /* no constraints yet */
        for (int i = 0; i < m->nsofts; i++) dropped[i] = 0;
        return 0;
    }

    memset(m->bb_drop, 0, (size_t)m->nsofts * sizeof *m->bb_drop);
    memset(m->bb_hit,  0, (size_t)m->ncores  * sizeof *m->bb_hit);
    /* initial feasible upper bound: drop everything */
    for (int i = 0; i < m->nsofts; i++) m->bb_best[i] = 1;

    bb_ctx ctx = { m, total };
    ms_bb(&ctx, 0, m->bb_hit);

    memcpy(dropped, m->bb_best, (size_t)m->nsofts * sizeof *m->bb_drop);
    return ctx.best;
}

/* ---- the IHS main loop --------------------------------------------------- */

int maxsat_solve(maxsat *m) {
    if (!sat_ok(m->s)) {
        /* A hard clause was empty the moment it was added: the hard rules
         * alone are contradictory (e.g. a package pinned to two versions). */
        m->solved = 1;
        return MAXSAT_UNSAT;
    }

    m->rounds    = 0;
    m->solved    = 0;
    m->best_cost = 0;
    m->nkept     = 0;
    m->ncores    = 0;             /* fresh run: forget old cores */
    m->pool_len  = 0;

    /* scratch for master + oracle */
    int  *S     = malloc((size_t)(m->nsofts ? m->nsofts : 1) * sizeof *S);
    int  *drop  = malloc((size_t)(m->nsofts ? m->nsofts : 1) * sizeof *drop);
    int  *core  = malloc((size_t)(m->nsofts ? m->nsofts : 1) * sizeof *core);
    if (!S || !drop || !core) { free(S); free(drop); free(core); m->solved = 1; return MAXSAT_UNSAT; }

    /* (Re)allocate per-soft B&B scratch (bb_hit/bb_mark grow with the cores
     * inside the loop). */
    if (m->cap_assumps < m->nsofts) m->cap_assumps = m->nsofts;
    {
        lit  *p1 = realloc(m->assumps, (size_t)(m->cap_assumps ? m->cap_assumps : 1) * sizeof *p1);
        int  *d1 = realloc(m->bb_drop, (size_t)(m->nsofts ? m->nsofts : 1) * sizeof *d1);
        int  *d2 = realloc(m->bb_best, (size_t)(m->nsofts ? m->nsofts : 1) * sizeof *d2);
        if (!p1 || !d1 || !d2) {
            m->assumps = p1; m->bb_drop = d1; m->bb_best = d2;
            free(S); free(drop); free(core); m->solved = 1; return MAXSAT_UNSAT;
        }
        m->assumps = p1; m->bb_drop = d1; m->bb_best = d2;
    }

    for (;;) {
        m->rounds++;

        /* ---- master: cheapest hitting set of all known cores ------------- */
        long cost = master_hitting_set(m, drop);
        mtrace(m, "master: cost=%ld dropped:", cost);
        if (mdbg()) {
            for (int i = 0; i < m->nsofts; i++) if (drop[i]) fprintf(stderr, " s%d(w=%ld)", i, m->softs[i].orig);
            fprintf(stderr, "\n");
        }

        /* ---- oracle: enforce everything NOT dropped ---------------------- */
        int nS = 0;
        for (int i = 0; i < m->nsofts; i++) if (!drop[i]) S[nS++] = i;

        int nc = extract_core(m, S, nS, core);

        if (nc == -1) {
            /* Feasible: the master's D is a minimum hitting set of every core,
             * and the true optimum must hit every core too — so cost(D) is the
             * OPTIMUM. The engine still holds the model (extract_core kept it),
             * so maxsat_value() reads a valid optimal assignment. */
            m->best_cost = cost;
            m->solved    = 1;
            int nkept = 0;
            for (int i = 0; i < m->nsofts; i++) if (!drop[i]) nkept++;
            m->nkept = nkept;
            free(S); free(drop); free(core);
            mtrace(m, "OPTIMAL cost=%ld kept=%d", cost, nkept);
            return MAXSAT_OPTIMAL;
        }
        if (nc == -2) { free(S); free(drop); free(core); m->solved = 1; return MAXSAT_UNSAT; }
        if (nc == 0) {
            /* Empty core with nothing enforced: the HARD clauses alone admit no
             * assignment at all (infeasible package request). */
            free(S); free(drop); free(core); m->solved = 1;
            return MAXSAT_UNSAT;
        }

        /* New core: record it. It forbids the current D (D misses C), so the
         * master will never pick D again — progress is guaranteed. */
        if (mdbg()) {
            fprintf(stderr, "[maxsat] new core n=%d members:", nc);
            for (int k = 0; k < nc; k++) fprintf(stderr, " s%d(w=%ld)", core[k], m->softs[core[k]].orig);
            fprintf(stderr, "\n");
        }
        if (!core_push(m, core, nc)) { free(S); free(drop); free(core); m->solved = 1; return MAXSAT_UNSAT; }

        /* grow bb_hit to the new core count */
        {
            int *h = realloc(m->bb_hit, (size_t)m->ncores * sizeof *h);
            if (!h) { free(S); free(drop); free(core); m->solved = 1; return MAXSAT_UNSAT; }
            m->bb_hit = h;
        }
        m->bb_hit[m->ncores - 1] = 0;
    }
}

/* ---- results ------------------------------------------------------------- */

long maxsat_cost(const maxsat *m)     { return m->best_cost; }
int  maxsat_remaining(const maxsat *m){ return m->nkept; }
long maxsat_rounds(const maxsat *m)   { return m->rounds; }

int maxsat_value(const maxsat *m, var v) {
    /* Collapse the engine's 3-valued answer to the boolean the caller wants
     * (an undetermined variable counts as false in an optimal model). */
    return sat_val_var(m->s, v) == VAL_TRUE ? 1 : 0;
}

void maxsat_stats(const maxsat *m, sat_stats *out) {
    if (out) sat_stats_get(m->s, out);
}

/* ---- debugging dump ------------------------------------------------------ */

/* Write the instance in WCNF (weighted partial CNF, the standard MaxSAT
 * interchange format):
 *
 *     p wcnf <nvars> <nclauses> <top>
 *     <top>  l_1 ... l_k 0        <- hard clause (weight = top)
 *     <wgt>  l_1 ... l_k 0        <- soft clause with its own weight
 *
 * Hard clauses are the engine's originals that contain no soft selector;
 * every soft was stored guarded as (~sel v l_1 ... v l_k), so we recognise it
 * by the selector variable and print the ORIGINAL lits with the soft weight.
 * Softs whose guarded clause was absorbed as a tautology by sat_add_clause
 * never appear — they are trivially satisfiable, so dropping them is
 * semantically exact.
 *
 * Returns the number of clauses written, or -1 on a bad argument. */
int maxsat_dump_wcnf(const maxsat *m, FILE *fp) {
    if (!m || !m->s || !fp) return -1;
    const solver *s = m->s;

    /* Top weight above the sum of every soft weight, so breaking any soft
     * clause always costs more than breaking all soft clauses together. */
    long top = 1;
    for (int i = 0; i < m->nsofts; i++)
        if (m->softs[i].orig > 0) top += m->softs[i].orig;

    /* selector variable -> soft index, to recover each soft's weight. */
    int nsvars = sat_nvars(s);
    int *sel_of = malloc((size_t)(nsvars > 0 ? nsvars : 1) * sizeof *sel_of);
    if (!sel_of) return -1;
    for (int i = 0; i < nsvars; i++) sel_of[i] = -1;
    for (int i = 0; i < m->nsofts; i++) {
        int v = m->softs[i].sel;
        if (v >= 0 && v < nsvars) sel_of[v] = i;
    }

    /* Count live clauses first so the header is exact. */
    int nlive = 0;
    for (int i = 0; i < s->norigs; i++)
        if (!s->origs[i]->dead) nlive++;
    int nclauses = s->nunits + nlive;

    fprintf(fp, "c planet wcnf dump: %d vars, %d clauses, top weight %ld\n"
                "c hard clauses carry weight top; soft carry their own\n",
            nsvars, nclauses, top);
    fprintf(fp, "p wcnf %d %d %ld\n", nsvars, nclauses, top);
    int written = 0;

    /* Units are hard facts: weight top. */
    for (int i = 0; i < s->nunits; i++) {
        lit l = s->units[i];
        fprintf(fp, "%ld %s%d 0\n", top, lit_sign(l) ? "-" : "",
                lit_var(l) + 1);
        written++;
    }

    /* Originals: soft if they carry a selector literal, hard otherwise. */
    for (int i = 0; i < s->norigs; i++) {
        clause *c = s->origs[i];
        if (c->dead) continue;
        int soft = -1;
        for (int k = 0; k < c->size && soft < 0; k++) {
            int v = lit_var(c->lits[k]);
            if (v >= 0 && v < nsvars && sel_of[v] >= 0) soft = sel_of[v];
        }
        long w = soft >= 0 ? m->softs[soft].orig : top;
        fprintf(fp, "%ld ", w);
        for (int k = 0; k < c->size; k++) {
            lit l = c->lits[k];
            /* The guard ~sel is an IHS implementation detail, not part of the
             * original soft clause: omit it from the dump. */
            int v = lit_var(l);
            if (soft >= 0 && v >= 0 && v < nsvars && sel_of[v] == soft)
                continue;
            fprintf(fp, "%s%d ", lit_sign(l) ? "-" : "", v + 1);
        }
        fprintf(fp, "0\n");
        written++;
    }

    free(sel_of);
    return written;
}
