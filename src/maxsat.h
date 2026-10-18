/*
 * maxsat.h — Weighted Partial MaxSAT, solved by a core-guided loop.
 * ==================================================================
 *
 * WHAT THIS IS
 * ------------
 * The CDCL engine in sat.c answers a yes/no question:
 *
 *     "Is there SOME assignment that satisfies all these clauses?"
 *
 * Dependency resolution needs more than "some" answer — it needs the BEST
 * one. Planet encodes the problem so that:
 *
 *   - HARD clauses  = rules that may never be broken
 *                     (Depends, Conflicts, Provides, Replaces, user pins)
 *   - SOFT clauses  = things we'd LIKE to hold, each with a WEIGHT
 *                     (user explicitly asked for X -> weight 1000,
 *                      prefer the newest version -> weight 10,
 *                      keep the footprint small -> weight 1)
 *
 * The goal of weighted partial MaxSAT is:
 *
 *     minimise   sum of weights of the FALSIFIED soft clauses
 *     subject to ALL hard clauses holding.
 *
 * In package terms: break as little of the user's wish-list as possible,
 * while never breaking a single hard rule.
 *
 * WHY "CORE-GUIDED"
 * -----------------
 * The naive approach — guess-and-check — is hopeless: you'd try combinations
 * until one worked. A core-guided loop instead LEARNS from failure:
 *
 *   1. Turn on every soft clause (assume all selectors true) and solve.
 *   2. If SAT, we satisfied everything still in play -> done, that's optimal.
 *   3. If UNSAT, ask the solver WHICH selectors caused the clash: the
 *      unsatisfiable core (sat_solve_assumptions gives us exactly this).
 *   4. From the core we learn an irrefutable fact:
 *          "at least one clause in this core must be dropped anyway"
 *      so we BANK the smallest weight in the core (it is guaranteed cost),
 *      REDUCE every member's weight by that amount, and add a clause that
 *      forbids the core from ever all-being-on again.
 *   5. Repeat. Each round the pool of positive-weight clauses shrinks, so the
 *      loop terminates, and when it does the banked cost is the provable
 *      OPTIMUM — not a heuristic guess.
 *
 * This is the reweighting core-guided algorithm (Fu–Malik lineage). It is
 * exact: `maxsat_cost()` returns the true minimum, and `maxsat_model()`
 * returns an assignment attaining it.
 *
 * API SHAPE
 * ---------
 * The usage pattern mirrors the SAT engine but adds weights:
 *
 *     maxsat *m = maxsat_new();
 *     var  p = maxsat_var(m);                // one variable per choice
 *     lit  hard[2] = { lit_mk(p,1), q };     // (~p v q)
 *     maxsat_hard(m, hard, 2);               // may never break
 *     lit  wish = lit_mk(p,0);
 *     maxsat_soft(m, &wish, 1, 1000);        // weight-1000 wish
 *     if (maxsat_solve(m) == MAXSAT_OPTIMAL) {
 *         long cost = maxsat_cost(m);        // provable minimum broken weight
 *         int  on   = maxsat_value(m, p);    // 1 => this choice is selected
 *     }
 *
 * LITERAL ENCODING: clause literals use the SAME encoding as the CDCL engine
 * (see lit_mk in sat.h): a non-negative int where bit0 is the sign and the
 * rest is the variable index. Always build them with lit_mk() — never with
 * raw +/- integers — because the engine indexes watch lists directly by the
 * literal value and a stray signed int would index out of bounds.
 *
 * CORRECTNESS CONTRACT
 * --------------------
 * maxsat_solve() returns MAXSAT_OPTIMAL and fills cost/model, or
 * MAXSAT_UNSAT when even the hard clauses alone cannot hold (in which case
 * there is no package set at all and the CLI must report an impossible
 * transaction rather than a broken one).
 */
#ifndef NEPTUMIUM_MAXSAT_H
#define NEPTUMIUM_MAXSAT_H

#include "sat.h"          /* for lit / var / lit_mk */
#include <stddef.h>

/* Result codes from maxsat_solve(). Distinct from the SAT engine's 0/1 so
 * callers never confuse "no model exists at all" with "optimal model found". */
#define MAXSAT_UNSAT   0   /* hard clauses alone are unsatisfiable */
#define MAXSAT_OPTIMAL 1   /* a provably optimal model is available */

typedef struct maxsat maxsat;

/* ---- lifecycle ---------------------------------------------------------- */
maxsat *maxsat_new(void);
void    maxsat_free(maxsat *m);

/* Allocate a fresh decision variable (shared ID space with the inner solver). */
var maxsat_var(maxsat *m);
int maxsat_nvars(const maxsat *m);

/* The inner CDCL engine. Exposed so the encoding layer can reuse the AMO
 * helpers in amo.h (they take a raw solver*), which add their auxiliary
 * variables and clauses directly into the SAME clause database the MaxSAT
 * loop will later solve — no bookkeeping duplication. Only add clauses to it
 * (never solve on it directly) while a MaxSAT problem is being built. */
solver *maxsat_engine(maxsat *m);

/* ---- clause registration ------------------------------------------------ */
/* Add a HARD clause: must be satisfied in every acceptable model.
 * `lits` uses the engine literal encoding (lit_mk). Returns 0 if the hard set
 * became trivially unsatisfiable (conflicting pins / contradictory Depends),
 * 1 otherwise. */
int maxsat_hard(maxsat *m, const lit *lits, int n);

/* Add a SOFT clause with the given positive weight (weight > 0).
 * `lits` uses the engine literal encoding (lit_mk). Internally the clause is
 * stored guarded by a fresh selector variable so the core-guided loop can
 * switch it on/off and observe the cost. Returns 0 on misuse (n < 1 or
 * weight <= 0), 1 on success. */
int maxsat_soft(maxsat *m, const lit *lits, int n, long weight);

/* ---- solve -------------------------------------------------------------- */
/* Run the core-guided loop to PROVABLE optimality.
 * Returns MAXSAT_OPTIMAL (cost + model valid until the next mutation) or
 * MAXSAT_UNSAT (the hard clauses alone are unsatisfiable). */
int  maxsat_solve(maxsat *m);

/* ---- results (valid after MAXSAT_OPTIMAL) ------------------------------- */
/* Minimum total weight of falsified soft clauses — the optimal broken cost. */
long maxsat_cost(const maxsat *m);
/* Truth of variable v in the optimal model (1 = true, 0 = false). */
int  maxsat_value(const maxsat *m, int v);
/* Number of soft clauses satisfied (kept, NOT paid for) in the optimal model.
 * 0 means every soft wish had to be broken. */
int  maxsat_remaining(const maxsat *m);
/* How many core rounds the loop ran — a direct measure of how tangled the
 * dependency problem was. Surfaced through `planet solve -S`. */
long maxsat_rounds(const maxsat *m);
/* Inner CDCL engine statistics, for `planet --solver-stats`. */
void maxsat_stats(const maxsat *m, sat_stats *out);

/* ---- debugging dump ------------------------------------------------------ */
/* Write the whole instance in WCNF (weighted partial CNF) form: hard clauses
 * with weight = top, soft clauses with their own weights, IHS guards removed.
 * Used by `planet dump-cnf` so an encoding bug can be reproduced in any MaxSAT
 * tool. Returns the number of clauses written, or -1 on a bad argument. */
int maxsat_dump_wcnf(const maxsat *m, FILE *fp);

#endif /* NEPTUMIUM_MAXSAT_H */
