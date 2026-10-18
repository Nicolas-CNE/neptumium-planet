#include "sat.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- small helpers ----------------------------------------------------- */

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "planet: out of memory\n"); exit(2); }
    return q;
}

/* growable pointer vector */
static void vp_push(vec_ptr *v, void *x) {
    if (v->size == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->data = xrealloc(v->data, (size_t)v->cap * sizeof(void *));
    }
    v->data[v->size++] = x;
}

/* ---- construction ------------------------------------------------------ */

solver *sat_new(void) {
    solver *s = calloc(1, sizeof *s);
    if (!s) exit(2);
    s->var_decay  = 0.95f;
    s->cla_decay  = 0.999f;
    s->var_inc    = 1.0f;
    s->cla_inc    = 1.0f;
    s->next_reduce = 2000;
    s->ok = 1;
    return s;
}

/* reserve capacity for at least `need` variables */
static void reserve_vars(solver *s, int need) {
    if (need <= s->cap_vars) return;
    int cap = s->cap_vars ? s->cap_vars : 8;
    while (cap < need) cap *= 2;

    s->assigns  = xrealloc(s->assigns,  (size_t)cap * sizeof *s->assigns);
    s->level    = xrealloc(s->level,    (size_t)cap * sizeof *s->level);
    s->reason   = xrealloc(s->reason,   (size_t)cap * sizeof *s->reason);
    s->activity = xrealloc(s->activity, (size_t)cap * sizeof *s->activity);
    s->seen     = xrealloc(s->seen,     (size_t)cap * sizeof *s->seen);
    s->seen_assump = xrealloc(s->seen_assump, (size_t)cap * sizeof *s->seen_assump);
    s->assump_lit  = xrealloc(s->assump_lit,  (size_t)cap * sizeof *s->assump_lit);
    s->is_assump   = xrealloc(s->is_assump,   (size_t)cap * sizeof *s->is_assump);

    /* trail_lim holds one entry per decision level (max nvars levels) */
    s->trail_lim = xrealloc(s->trail_lim, (size_t)(cap + 1) * sizeof *s->trail_lim);

    /* watch array must cover literals 0..2*cap-1 */
    s->watches = xrealloc(s->watches, (size_t)(2 * cap) * sizeof *s->watches);
    for (int i = 2 * s->cap_vars; i < 2 * cap; i++) {
        s->watches[i].data = NULL;
        s->watches[i].size = 0;
        s->watches[i].cap  = 0;
    }

    for (int v = s->cap_vars; v < cap; v++) {
        s->assigns[v]  = VAL_UNDEF;
        s->level[v]    = -1;
        s->reason[v]   = NULL;
        s->activity[v] = 0.0f;
        s->seen[v]     = 0;
        s->seen_assump[v] = 0;
        s->is_assump[v]   = 0;
    }
    s->cap_vars = cap;
}

var sat_add_var(solver *s) {
    reserve_vars(s, s->nvars + 1);
    return s->nvars++;
}

int sat_nvars(const solver *s) { return s->nvars; }
int sat_ok(const solver *s)    { return s->ok; }

void sat_free(solver *s) {
    if (!s) return;
    free(s->assigns); free(s->level); free(s->reason);
    free(s->activity); free(s->seen); free(s->seen_assump);
    free(s->assump_lit); free(s->is_assump);
    free(s->trail); free(s->trail_lim);
    for (int i = 0; i < 2 * s->cap_vars; i++) free(s->watches[i].data);
    free(s->watches);
    for (int i = 0; i < s->nlearnts; i++) free(s->learnts[i]);
    free(s->learnts);
    /* Original clauses are permanent (never deleted by reduce_db), so every
     * pointer in origs is live and owned here. */
    for (int i = 0; i < s->norigs; i++) free(s->origs[i]);
    free(s->origs);
    free(s->units);
    free(s);
}

/* ---- trail / assignment ------------------------------------------------ */

/* Make literal `p` true at the current decision level. reason may be NULL
 * (decision or top-level axiom). Returns 0 on immediate conflict. */
static int enqueue(solver *s, lit p, clause *reason) {
    var v = lit_var(p);
    val want = lit_sign(p) ? VAL_FALSE : VAL_TRUE;
    if (s->assigns[v] != VAL_UNDEF)
        return s->assigns[v] == want;          /* already consistent? */
    s->assigns[v] = want;
    s->level[v]   = s->decision_level;
    s->reason[v]  = reason;
    if (s->trail_sz == s->trail_cap) {
        s->trail_cap = s->trail_cap ? s->trail_cap * 2 : 1024;
        s->trail = xrealloc(s->trail, (size_t)s->trail_cap * sizeof *s->trail);
    }
    s->trail[s->trail_sz++] = p;
    return 1;
}

/* value of a literal given current assignment */
static val lit_val(const solver *s, lit p) {
    val a = s->assigns[lit_var(p)];
    if (a == VAL_UNDEF) return VAL_UNDEF;
    return lit_sign(p) ? (a == VAL_TRUE ? VAL_FALSE : VAL_TRUE) : a;
}

/* ---- watch bookkeeping ------------------------------------------------- */

static void watch_add(solver *s, lit p, clause *c) {
    vp_push(&s->watches[p], c);
}

/* Add a clause whose lits[0],lits[1] are the watched pair. */
static clause *clause_new(solver *s, lit *lits, int n, int learnt) {
    clause *c = xrealloc(NULL, sizeof *c + (size_t)n * sizeof *lits);
    c->learnt = learnt;
    c->dead   = 0;
    c->prot   = 0;
    c->size   = n;
    c->act    = 0.0f;
    memcpy(c->lits, lits, (size_t)n * sizeof *lits);
    watch_add(s, c->lits[0], c);
    watch_add(s, c->lits[1], c);
    if (!learnt) {
        /* Remember originals so sat_free can release them and so dumpers can
         * iterate the problem statement (see the origs comment in sat.h). */
        if (s->norigs == s->cap_origs) {
            s->cap_origs = s->cap_origs ? s->cap_origs * 2 : 64;
            s->origs = xrealloc(s->origs, (size_t)s->cap_origs * sizeof *s->origs);
        }
        s->origs[s->norigs++] = c;
        s->nclauses++;
    }
    return c;
}

static void clause_bump(solver *s, clause *c) {
    c->act += s->cla_inc;
    if (c->act > 1e20f) {
        for (int i = 0; i < s->nlearnts; i++) s->learnts[i]->act *= 1e-20f;
        s->cla_inc *= 1e-20f;
    }
}

/* ---- clause insertion (public) ----------------------------------------- */

int sat_add_clause(solver *s, lit *lits, int n) {
    if (!s->ok) return 0;

    /* normalize: drop duplicates, detect tautology, collect */
    /* small stack buffer for typical sizes, heap if huge */
    lit *buf = NULL, tmp[64];
    if (n <= 64) buf = tmp;
    else buf = xrealloc(NULL, (size_t)n * sizeof *lits);

    /* sort to dedupe / tautology-detect */
    memcpy(buf, lits, (size_t)n * sizeof *lits);
    /* simple insertion sort (n is small for clauses) */
    for (int i = 1; i < n; i++) {
        lit k = buf[i]; int j = i - 1;
        while (j >= 0 && buf[j] > k) { buf[j+1] = buf[j]; j--; }
        buf[j+1] = k;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && buf[i] == buf[i-1]) continue;      /* dup */
        if (m > 0 && buf[m-1] == lit_neg(buf[i])) {     /* tautology */
            if (buf != tmp) free(buf);
            return 1;
        }
        buf[m++] = buf[i];
    }

    int rc = 1;
    if (m == 0) {
        s->ok = 0; rc = 0;                              /* empty clause */
    } else if (m == 1) {
        /* Permanent unit clause: remember it so it survives a reset to the
         * root state (MaxSAT calls solve() repeatedly from a clean slate). */
        if (s->nunits == s->cap_units) {
            s->cap_units = s->cap_units ? s->cap_units * 2 : 16;
            s->units = xrealloc(s->units, (size_t)s->cap_units * sizeof *s->units);
        }
        s->units[s->nunits++] = buf[0];
        if (!enqueue(s, buf[0], NULL)) { s->ok = 0; rc = 0; }
    } else {
        clause_new(s, buf, m, 0);
    }
    if (buf != tmp) free(buf);
    return rc;
}

/* ---- backtracking ------------------------------------------------------ */

static void cancel_until(solver *s, int lvl) {
    if (s->decision_level <= lvl) return;
    int c = s->trail_lim[lvl];
    while (s->trail_sz > c) {
        lit p = s->trail[--s->trail_sz];
        var v = lit_var(p);
        s->assigns[v] = VAL_UNDEF;
        s->reason[v]  = NULL;
        s->level[v]   = -1;
        /* re-polarity: keep phase for next time (positive default) */
    }
    s->qhead = s->trail_sz;
    s->decision_level = lvl;
}

/* ---- BCP: 2-watched-literal propagation -------------------------------- */

static clause *propagate(solver *s) {
    clause *confl = NULL;
    while (s->qhead < s->trail_sz) {
        lit p = s->trail[s->qhead++];     /* p just became true */
        s->propagations++;
        lit false_lit = lit_neg(p);       /* this literal just became false */
        vec_ptr *ws = &s->watches[false_lit];

        int i = 0, j = 0;
        while (i < ws->size) {
            clause *c = (clause *)ws->data[i++];

            /* ensure false_lit is at position 1 for a stable invariant:
             * we keep the *new* watched literal at position 0. */
            if (c->lits[0] == false_lit) {
                c->lits[0] = c->lits[1];
                c->lits[1] = false_lit;
            }
            lit other = c->lits[0];

            if (lit_val(s, other) == VAL_TRUE) {
                ws->data[j++] = c;                     /* satisfied, keep */
                continue;
            }

            /* look for a non-false literal to watch instead */
            int k, repl = -1;
            for (k = 2; k < c->size; k++) {
                if (lit_val(s, c->lits[k]) != VAL_FALSE) { repl = k; break; }
            }
            if (repl >= 0) {
                /* --- Replace the FALSE watch, keep the live one ---------------
                 * On entry (after the swap above) the watched pair is
                 *   lits[0] = other   (still usable: not TRUE, but NOT false
                 *                      either — or we'd have taken the
                 *                      unit/conflict path below)
                 *   lits[1] = false_lit (the literal that just became FALSE)
                 * and the clause sits in watches[other] and watches[false_lit].
                 *
                 * false_lit is now permanently false (until a backjump), so it
                 * is a useless watch. We swap it OUT for the freshly found
                 * non-false literal N = lits[repl], and move false_lit down to
                 * position `repl` (an unwatched slot). Position 0 (`other`) is
                 * left untouched.
                 *
                 * This keeps the literal array in sync with the watch lists:
                 *   watches[other]      -> kept   (still lits[0])
                 *   watches[N]          -> added  (watch_add below, now lits[1])
                 *   watches[false_lit]  -> dropped (we skip copying to j)
                 * so the invariant  {lits[0], lits[1]} == watch-list membership
                 * holds. Getting this backwards (replacing lits[0] and leaving
                 * false_lit at lits[1]) leaves a STALE entry in watches[other]
                 * while the clause vanishes from watches[false_lit]; later BCP
                 * then re-reads the clause against the wrong watched pair and
                 * can produce a spurious conflict, yielding a false UNSAT that
                 * persists across solve() calls. */
                c->lits[1]    = c->lits[repl];
                c->lits[repl] = false_lit;
                watch_add(s, c->lits[1], c);           /* move to new list */
                /* drop from current list (do not copy to j) */
                continue;
            }

            /* no replacement -> clause is unit or conflicting */
            if (lit_val(s, other) == VAL_FALSE) {
                /* conflict: keep remaining watchers, report */
                ws->data[j++] = c;
                while (i < ws->size) ws->data[j++] = ws->data[i++];
                ws->size = j;
                return c;
            } else {
                ws->data[j++] = c;                     /* keep watching */
                if (!enqueue(s, other, c)) {
                    /* enqueue conflict (shouldn't normally happen) */
                    while (i < ws->size) ws->data[j++] = ws->data[i++];
                    ws->size = j;
                    return c;
                }
            }
        }
        ws->size = j;
    }
    return confl;
}

/* ---- VSIDS ------------------------------------------------------------- */

static void var_bump(solver *s, var v) {
    s->activity[v] += s->var_inc;
    if (s->activity[v] > 1e30f) {
        for (int i = 0; i < s->nvars; i++) s->activity[i] *= 1e-30f;
        s->var_inc *= 1e-30f;
    }
}
static void var_decay(solver *s) { s->var_inc *= (1.0f / s->var_decay); }

/* pick an unassigned var with highest VSIDS activity */
static var pick_branch(const solver *s) {
    var best = VAR_UNDEF;
    float ba = -1.0f;
    for (var v = 0; v < s->nvars; v++) {
        if (s->assigns[v] == VAL_UNDEF && s->activity[v] > ba) {
            ba = s->activity[v];
            best = v;
        }
    }
    return best;
}

/* ---- conflict analysis (1UIP) ------------------------------------------ */

/* Analyze conflict `confl`. Produces learnt clause in out_lits (size returned),
 * with out_lits[0] the asserting literal. Sets *out_bt = backjump level. */
static int analyze(solver *s, clause *confl, lit *out_lits, int *out_bt) {
    int pathC = 0;
    lit p = LIT_UNDEF;
    int nlearnt = 0;
    int idx = s->trail_sz - 1;

    do {
        clause *c = (p == LIT_UNDEF) ? confl : s->reason[lit_var(p)];
        /* A variable with no reason is a DECISION (or a unit / assumption):
         * there is nothing to resolve through, so we only use it as a cut
         * point. MiniSAT guards this with `if (c != CRef_Undef)`; without the
         * guard we dereference a NULL reason and crash. */
        if (c) {
            clause_bump(s, c);
            int start = (p == LIT_UNDEF) ? 0 : 1;   /* skip p itself in reason */
            for (int i = start; i < c->size; i++) {
                lit q = c->lits[i];
                var v = lit_var(q);
                if (!s->seen[v] && s->level[v] > 0) {
                    s->seen[v] = 1;
                    var_bump(s, v);
                    if (s->level[v] >= s->decision_level) pathC++;
                    else out_lits[nlearnt++] = q;
                }
            }
        }
        /* advance to next seen var on trail */
        do { p = s->trail[idx--]; } while (!s->seen[lit_var(p)]);
        s->seen[lit_var(p)] = 0;
        pathC--;
    } while (pathC > 0);

    /* Place asserting literal lit_neg(p) at index 0; the non-UIP literals
     * collected above (out_lits[0..nlearnt-1]) follow it. */
    lit assert_lit = lit_neg(p);
    memmove(out_lits + 1, out_lits, (size_t)nlearnt * sizeof *out_lits);
    out_lits[0] = assert_lit;
    nlearnt++;

    /* backjump level = second-highest decision level in the learnt clause */
    int bt = 0;
    for (int i = 1; i < nlearnt; i++) {
        int lv = s->level[lit_var(out_lits[i])];
        if (lv > bt) bt = lv;
    }
    /* clear seen for learnt lits */
    for (int i = 0; i < nlearnt; i++) s->seen[lit_var(out_lits[i])] = 0;
    *out_bt = bt;
    return nlearnt;
}

/* ---- Luby restart sequence --------------------------------------------- */

static long luby(long y, long x) {
    long size, seq;
    for (size = 1, seq = 0; size < x + 1; seq++, size = 2 * size + 1) ;
    while (size - 1 != x) {
        size = (size - 1) >> 1;
        seq--;
        x = x % size;
    }
    return 1L << seq;
}

/* ---- clause-DB reduction ----------------------------------------------- */

static int cmp_act_desc(const void *a, const void *b) {
    const clause *ca = *(clause *const *)a;
    const clause *cb = *(clause *const *)b;
    if (ca->act > cb->act) return -1;
    if (ca->act < cb->act) return 1;
    return 0;
}

static void reduce_db(solver *s) {
    if (s->nlearnts < 4) return;

    /* Protect every learnt clause that is currently the REASON of an assigned
     * variable on the trail. Freeing one would leave a dangling reason pointer
     * that analyze() later dereferences; the MaxSAT deletion filter makes
     * reduce_db run frequently, which is what exposed the defect. */
    for (int i = 0; i < s->nlearnts; i++) s->learnts[i]->prot = 0;
    for (int i = 0; i < s->trail_sz; i++) {
        clause *r = s->reason[lit_var(s->trail[i])];
        if (r) r->prot = 1;
    }

    qsort(s->learnts, (size_t)s->nlearnts, sizeof *s->learnts, cmp_act_desc);
    int keep = s->nlearnts / 2;
    for (int i = keep; i < s->nlearnts; i++)
        if (!s->learnts[i]->prot) s->learnts[i]->dead = 1;

    /* sweep dead clauses out of every watch list */
    for (int l = 0; l < 2 * s->nvars; l++) {
        vec_ptr *ws = &s->watches[l];
        int j = 0;
        for (int i = 0; i < ws->size; i++) {
            clause *c = (clause *)ws->data[i];
            if (!c->dead) ws->data[j++] = c;
        }
        ws->size = j;
    }
    /* compact learnts + free dead */
    int w = 0;
    for (int i = 0; i < s->nlearnts; i++) {
        clause *c = s->learnts[i];
        if (c->dead) free(c);
        else s->learnts[w++] = c;
    }
    s->nlearnts = w;
}

/* ---- main search loop -------------------------------------------------- */

/* Extract an UNSAT core from a root-level conflict.
 *
 * Walks backward from the conflicting clause through the implication graph
 * (via `reason`) collecting every variable that is an *assumption selector*
 * still marked relevant. The result is the subset of assumptions that were
 * actually required to derive the conflict — exactly what the core-guided
 * MaxSAT reweighting loop consumes. Non-minimal but sound, which is all the
 * algorithm needs.
 *
 * On return, *core_n assumption literals have been written to core_out. */
static void analyze_final(solver *s, clause *confl, lit *core_out, int *core_n) {
    int n = 0;

    /* Seed: every variable in the conflicting clause is relevant. */
    for (int i = 0; i < confl->size; i++)
        s->seen_assump[lit_var(confl->lits[i])] = 1;

    /* Walk the trail top-down; expand reasons of relevant vars, and harvest
     * the ones that are assumption selectors. */
    for (int i = s->trail_sz - 1; i >= 0; i--) {
        lit  p = s->trail[i];
        var  v = lit_var(p);
        if (!s->seen_assump[v]) continue;
        s->seen_assump[v] = 0;                 /* process each var once */
        clause *r = s->reason[v];
        if (r == NULL) {
            /* A decision/root assignment. If it's an assumption, it belongs
             * in the core. (Permanent unit clauses have is_assump == 0 and
             * are correctly excluded — they're hard facts, not assumptions.) */
            if (s->is_assump[v]) core_out[n++] = s->assump_lit[v];
        } else {
            for (int k = 0; k < r->size; k++)
                s->seen_assump[lit_var(r->lits[k])] = 1;
        }
    }
    *core_n = n;
}

/* The core CDCL search. Assumes units + any assumptions are already enqueued
 * and propagated has NOT yet run for the current trail tail. On a root-level
 * conflict, extracts an UNSAT core into core_out (if non-NULL). */
static int search(solver *s, lit *core_out, int *core_n) {
    if (core_n) *core_n = 0;

    /* Luby restart budget: restart after `restart_budget` conflicts. */
    const long restart_base = 100;
    long restart_budget = restart_base * luby(2, 0);
    long conflicts_at_last_restart = 0;

    for (;;) {
        clause *confl = propagate(s);
        if (confl) {
            s->conflicts++;
            if (s->decision_level == 0) {
                /* Root-level conflict: the formula + assumptions are UNSAT.
                 * Harvest the core for the MaxSAT loop. */
                if (core_out) analyze_final(s, confl, core_out, core_n);
                return 0;                       /* UNSAT */
            }

            lit *learnt = xrealloc(NULL, (size_t)(s->nvars + 1) * sizeof *learnt);
            int bt;
            int n = analyze(s, confl, learnt, &bt);
            cancel_until(s, bt);

            if (n == 1) {
                enqueue(s, learnt[0], NULL);
            } else {
                /* watch asserting literal (pos 0) and one other (pos 1) */
                clause *c = clause_new(s, learnt, n, 1);
                s->learnts = xrealloc(s->learnts,
                                      (size_t)(s->nlearnts + 1) * sizeof *s->learnts);
                s->learnts[s->nlearnts++] = c;
                s->learnts_total++;
                enqueue(s, learnt[0], c);
            }
            free(learnt);
            var_decay(s);

            /* periodically shrink the learnt DB */
            if (s->conflicts % s->next_reduce == 0) reduce_db(s);

            /* Luby restart when the conflict budget since last restart is spent */
            if (s->conflicts - conflicts_at_last_restart >= restart_budget) {
                cancel_until(s, 0);
                s->restarts++;
                conflicts_at_last_restart = s->conflicts;
                restart_budget = restart_base * luby(2, s->restarts);
            }

        } else {
            var v = pick_branch(s);
            if (v == VAR_UNDEF) return 1;              /* all assigned: SAT */
            s->decisions++;
            s->trail_lim[s->decision_level] = s->trail_sz;
            s->decision_level++;
            /* default phase: assign positive */
            enqueue(s, lit_mk(v, 0), NULL);
        }
    }
}

/* Undo every assignment, returning the solver to a clean root state with only
 * the permanent unit clauses (re)enqueued. This lets MaxSAT call solve() many
 * times against the same hard-clause database with different assumptions. */
static void reset_to_clean(solver *s) {
    while (s->trail_sz > 0) {
        lit p = s->trail[--s->trail_sz];
        var v = lit_var(p);
        s->assigns[v] = VAL_UNDEF;
        s->reason[v]  = NULL;
        s->level[v]   = -1;
    }
    s->qhead = 0;
    s->decision_level = 0;
    for (int i = 0; i < s->nunits; i++) enqueue(s, s->units[i], NULL);
    if (s->cap_vars > 0)
        s->trail_lim[0] = s->trail_sz;      /* level-0 section ends here */
}

/* Drop every learned clause.
 *
 * WHY THIS IS NECESSARY: a clause learned during a solve is implied by
 * (hard clauses ^ the assumptions active in THAT solve), because `analyze`
 * treats level-0 assumptions as background facts and never records their
 * negation. Such a clause is sound while those assumptions hold, but reusing
 * it under a DIFFERENT assumption set can wrongly force UNSAT: the stale
 * clause acts as an extra hard rule that the new assumption set does not
 * imply. This produced false UNSAT answers on instances the brute-force
 * oracle found satisfiable.
 *
 * So we never carry learned clauses across solve() calls. Within a single
 * solve the assumption set is fixed (and never backtracked past level 0), so
 * in-solve learning remains sound. The cost is re-learning per solve, which
 * is the speed-for-correctness trade this layer makes deliberately.
 *
 * SAFETY: call this only right after reset_to_clean(), when the trail holds
 * nothing but units enqueued with reason==NULL — so no reason pointer can
 * reference a clause we are about to free. */
static void clear_learnts(solver *s) {
    if (s->nlearnts == 0) return;
    /* Remove learned clauses from every watch list, keeping hard clauses. */
    for (int l = 0; l < 2 * s->nvars; l++) {
        vec_ptr *ws = &s->watches[l];
        int j = 0;
        for (int i = 0; i < ws->size; i++) {
            clause *c = (clause *)ws->data[i];
            if (!c->learnt) ws->data[j++] = c;
        }
        ws->size = j;
    }
    for (int i = 0; i < s->nlearnts; i++) free(s->learnts[i]);
    s->nlearnts = 0;
}

int sat_solve_assumptions(solver *s, const lit *assumptions, int n,
                          lit *core_out, int *core_n) {
    if (core_n) *core_n = 0;
    if (!s->ok) return 0;               /* hard clauses alone are UNSAT */

    reset_to_clean(s);
    clear_learnts(s);                   /* never reuse clauses learned under
                                         * a previous assumption set */
    s->solve_calls++;

    /* Tag each assumption's variable so core extraction can recognize it. */
    for (int i = 0; i < n; i++) {
        var v = lit_var(assumptions[i]);
        s->is_assump[v]   = 1;
        s->assump_lit[v]  = assumptions[i];
    }
    s->assumptions   = assumptions;
    s->nassumptions  = n;

    /* Enqueue the assumptions at level 0 as if they were unit clauses. If one
     * is directly contradicted by an already-derived literal, that contradiction
     * IS a (tiny) conflict we can harvest a core from immediately. */
    for (int i = 0; i < n; i++) {
        if (!enqueue(s, assumptions[i], NULL)) {
            /* assumptions[i] is falsified by current deductions. Build a
             * single-literal conflict from it and extract the core. */
            lit fake = assumptions[i];
            clause *tmp = xrealloc(NULL, sizeof *tmp + sizeof(lit));
            tmp->learnt = 0; tmp->dead = 0; tmp->size = 1; tmp->act = 0;
            tmp->lits[0] = fake;
            if (core_out) analyze_final(s, tmp, core_out, core_n);
            free(tmp);
            s->nassumptions = 0;
            for (int k = 0; k < n; k++) s->is_assump[lit_var(assumptions[k])] = 0;
            /* Leave the trail clean: a UNSAT result has no model to preserve,
             * and the caller (MaxSAT) will immediately add a new core clause.
             * Adding it onto a trail that still holds these assumptions would
             * make enqueue() collide with a stale assignment and wrongly mark
             * the whole solver UNSAT (ok=0). Resetting to units-only state
             * lets the new clause be added against a consistent base. */
            reset_to_clean(s);
            return 0;
        }
    }

    int rc = search(s, core_out, core_n);

    /* Untag assumption variables and drop the borrowed pointer. */
    for (int i = 0; i < n; i++) s->is_assump[lit_var(assumptions[i])] = 0;
    s->assumptions  = NULL;
    s->nassumptions = 0;

    /* Same rationale as above: after UNSAT, hand back a clean units-only
     * trail so the caller can extend the formula safely. On SAT we keep the
     * trail intact so sat_val_var() still reads the satisfying assignment. */
    if (rc == 0) reset_to_clean(s);
    return rc;
}

int sat_solve(solver *s) {
    return sat_solve_assumptions(s, NULL, 0, NULL, NULL);
}

void sat_reset(solver *s) {
    reset_to_clean(s);
}

/* ---- DEBUG: watch-list invariant ----------------------------------------
 * A clause must be present in exactly the watch lists of its two watched
 * literals (lits[0] and lits[1]). If a clause sits in watches[lit] where lit
 * is NOT one of its watched pair, that is a stale entry left behind by a
 * previous propagation — it makes later BCP process the clause against the
 * wrong watched literal, which produces spurious conflicts. (A defect of this
 * kind previously caused false UNSAT answers that persisted across solve()
 * calls.) Returns the number of violations found; prints them when verbose. */
int sat_debug_watch_check(solver *s, int verbose) {
    int viol = 0;
    for (int l = 0; l < 2 * s->nvars; l++) {
        vec_ptr *ws = &s->watches[l];
        for (int i = 0; i < ws->size; i++) {
            clause *c = (clause *)ws->data[i];
            if (c->lits[0] != l && c->lits[1] != l) {
                viol++;
                if (verbose)
                    fprintf(stderr,
                        "  STALE watch: clause (size %d) sits in watches[lit %d] "
                        "but its watched pair is [%d,%d]\n",
                        c->size, l, c->lits[0], c->lits[1]);
            }
        }
    }
    return viol;
}

/* ---- post-solve query -------------------------------------------------- */

val sat_val_var(const solver *s, var v) { return s->assigns[v]; }

void sat_stats_get(const solver *s, sat_stats *out) {
    out->conflicts    = s->conflicts;
    out->decisions    = s->decisions;
    out->propagations = s->propagations;
    out->restarts     = s->restarts;
    out->learnts      = s->nlearnts;
    out->learnts_total = s->learnts_total;
    out->solve_calls   = s->solve_calls;
    out->nvars         = s->nvars;
    out->nclauses      = s->nclauses;
    out->vsids_decay  = s->var_decay;
}

void sat_print_stats(FILE *fp, const sat_stats *st) {
    fprintf(fp,
        "c decisions    : %ld\n"
        "c propagations : %ld\n"
        "c conflicts    : %ld\n"
        "c restarts     : %ld\n"
        "c learnts live : %ld\n"
        "c learnts total: %ld\n"
        "c solve calls  : %ld\n"
        "c vars/clauses : %d / %d\n"
        "c vsids decay  : %.3f\n",
        st->decisions, st->propagations, st->conflicts,
        st->restarts, st->learnts, st->learnts_total, st->solve_calls,
        st->nvars, st->nclauses, (double)st->vsids_decay);
}
