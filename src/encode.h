/*
 * encode.h — packages -> weighted partial MaxSAT.
 * ===========================================================
 *
 * This layer turns a dependency-resolution problem into an exact optimization
 * problem, hands it to the core-guided solver, and returns an optimal package
 * set.
 *
 * THE ENCODING (see encode.c for the full derivation)
 * ---------------------------------------------------
 * One boolean variable per CANDIDATE (a specific name@version in the repo):
 *
 *   x_c = 1   <=>  candidate c is part of the chosen set.
 *
 * HARD clauses (violating any of these = an invalid system):
 *   1. At-most-one version per package name  (sequential ladder AMO, O(N)
 *      clauses even for repos with many versions of one package).
 *   2. Depends:  x_c -> (some provider of c's dependency is chosen).
 *   3. Conflicts AND Replaces: x_c -> (no conflicting candidate is chosen).
 *      Both are NON-COEXISTENCE bans. Conflicts because the packages fight
 *      over files/role; Replaces because the replacement takes over the old
 *      package's files and role, so the old one must be removed. In Arch the
 *      usual rename idiom declares BOTH on the new package — neither
 *      suppresses the other, so we emit the same clause for either relation.
 *   Provides does not get its own clause — it is folded into the *provider
 *   lookup* used by (2) and (3): a dependency on a virtual name is discharged
 *   by anything that Provides it.
 *
 * WEIGHTED soft clauses (each broken one costs its weight; solver minimises
 * the total broken weight):
 *   A. User request:   (some provider of the request)   weight = W_REQUEST
 *                      high, so explicit wishes dominate.
 *   B. Footprint + version preference, encoded together as ONE family:
 *        for each candidate c of a name, sorted NEWEST first, rank r = 0,1,2...
 *        soft clause  (not x_c)   weight = W_FOOTPRINT + W_VERSION * r
 *      Reading it: making x_c true BREAKS the clause and costs that weight.
 *      So installing the newest candidate of a name costs only the small
 *      footprint base; installing an older one costs the base PLUS r steps;
 *      installing nothing costs 0.  Three consequences fall out for free:
 *        - unneeded packages stay uninstalled (only footprint pushes);
 *        - when a name must be installed (hard dep or request), the cheapest
 *          feasible candidate is the NEWEST one  =>  "prefer latest";
 *        - if the newest candidate is infeasible (its own dependencies
 *          cannot be satisfied)
 *          the hard clauses simply forbid it and the solver drops to the
 *          newest *feasible* version automatically.
 *      This single family replaces what would otherwise be two separate,
 *      interacting objectives (footprint vs. newest), keeping the clause
 *      count linear in the number of candidates.
 *
 * WEIGHT TIERS (encode_weight_t). The three tiers must satisfy
 *   W_REQUEST  >  W_VERSION * (max versions of a name) + W_FOOTPRINT
 * so a request is never dropped in favour of "cheaper to not install", and
 *   W_VERSION  >  0
 * so newer versions always win among feasible ones. Defaults are far apart
 * (10000 / 100 / 1) to leave room for repos with deep version stacks.
 *
 * WHY EXACTNESS MATTERS: a greedy resolver returns the first feasible
 * solution; this returns the minimum-cost one, so `planet solve` can state
 * that no cheaper package set exists and report the exact soft-clause cost in
 * `--solver-stats`.
 */
#ifndef NEPTUMIUM_ENCODE_H
#define NEPTUMIUM_ENCODE_H

#include "pkg.h"
#include "maxsat.h" /* MAXSAT_OPTIMAL / MAXSAT_UNSAT return codes */
#include "sat.h"   /* sat_stats, for --solver-stats pass-through */

/* ---- weight tiers -------------------------------------------------------- */

typedef struct {
    long request;     /* weight of satisfying a user request   (default 10000) */
    long version;     /* per-rank penalty for older versions   (default 100)   */
    long footprint;   /* base penalty for installing anything  (default 1)     */
} enc_weights;

/* The tier defaults documented above. */
enc_weights enc_default_weights(void);

/* ---- encoder ------------------------------------------------------------- */

typedef struct encoder encoder;

/* Create an encoder over an already-populated repo (the encoder does NOT take
 * ownership of the repo; the caller frees it after enc_free). */
encoder *enc_new(const repo *r);

/* Override the weight tiers (call before enc_solve; after is ignored). */
void enc_set_weights(encoder *e, enc_weights w);

/* Register a user request: "install something providing (name, op, ver)".
 * Returns 1 if at least one candidate in the repo can satisfy it, 0 if the
 * name is simply absent (caller should report "not found" — there is nothing
 * to optimise). `weight` <= 0 means "use the W_REQUEST tier". */
int enc_request(encoder *e, const char *name, dep_op op, const char *ver,
                long weight);

/* Register a HARD requirement: "the final set MUST contain something providing
 * (name, op, ver)". Unlike enc_request (a soft wish), this becomes a hard
 * clause the solver cannot break — used by remove/upgrade to pin packages that
 * must stay installed. Returns 1 if satisfiable, 0 if the name is absent. */
int enc_require(encoder *e, const char *name, dep_op op, const char *ver);

/* Build the whole MaxSAT problem and solve it.
 * Returns MAXSAT_OPTIMAL (model + cost valid) or MAXSAT_UNSAT (even the hard
 * rules alone admit no consistent system — e.g. two hard-pinned versions of
 * the same package). */
int enc_solve(encoder *e);

/* Export the exact instance that enc_solve() would run, in WCNF form, WITHOUT
 * solving it. Returns the number of clauses written or -1 on failure.
 * `planet dump-cnf` uses this to make encoding bugs reproducible in any
 * MaxSAT tool. */
int enc_dump_cnf(encoder *e, FILE *fp);

/* ---- results (valid after MAXSAT_OPTIMAL) -------------------------------- */

int         enc_ncandidates(const encoder *e);
/* The i-th candidate in repo order; NULL if i out of range. */
const package *enc_candidate(const encoder *e, int i);
/* 1 if candidate i was chosen in the optimal set. */
int         enc_chosen(const encoder *e, int i);

/* Total broken soft weight of the optimal solution (== maxsat_cost). */
long        enc_cost(const encoder *e);
/* How many of the registered user requests the optimal set actually satisfies
 * (a request can be "unmet" if its providers are all hard-forced out). */
int         enc_satisfied(const encoder *e);
/* Core rounds + inner CDCL stats, for `planet solve -S`. */
long        enc_rounds(const encoder *e);
void        enc_stats(const encoder *e, sat_stats *out);

/* Was request k satisfied by the optimal set? (k = index into the requests as
 * added; returns 0 if k out of range or the solve failed.) Used by the CLI to
 * print "requested foo but it could not be installed". */
int         enc_request_met(const encoder *e, int k);

void enc_free(encoder *e);

#endif /* NEPTUMIUM_ENCODE_H */
