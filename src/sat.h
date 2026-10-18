#ifndef SAT_H
#define SAT_H
/* Planet - CDCL SAT/MaxSAT core.
 * From-scratch CDCL engine: 2-watched-literal BCP, VSIDS decisions,
 * 1UIP clause learning, Luby restarts, activity-based clause-DB reduction.
 */
#include <stdint.h>
#include <stdio.h>

/* ---- literals & variables ---------------------------------------------- */
typedef int32_t lit;
typedef int32_t var;

#define LIT_UNDEF ((lit)-1)
#define VAR_UNDEF ((var)-1)

static inline var lit_var(lit l)  { return l >> 1; }
static inline int lit_sign(lit l) { return l & 1; }        /* 1 == negated */
static inline lit lit_neg(lit l)  { return l ^ 1; }
static inline lit lit_mk(var v, int neg) { return (v << 1) | (neg ? 1 : 0); }

typedef enum { VAL_FALSE = 0, VAL_TRUE = 1, VAL_UNDEF = 2 } val;

/* ---- clause ------------------------------------------------------------
 * Variable-length: literal array is a C11 flexible member. lits[0] and lits[1]
 * are always the two watched literals.
 */
typedef struct clause {
    int   learnt;
    int   dead;
    int   size;
    int   prot;         /* protected: currently the reason of a trail var —
                         * reduce_db must NOT free it even if its activity is
                         * low, otherwise analyze() bumps a dangling pointer. */
    float act;
    lit   lits[];
} clause;

/* growable pointer vector (watch lists) */
typedef struct { void **data; int size, cap; } vec_ptr;

/* ---- solver ------------------------------------------------------------ */
typedef struct solver {
    /* per-variable state */
    int     nvars, cap_vars;
    val    *assigns;
    int    *level;         /* decision level of var (-1 unassigned) */
    clause **reason;      /* implying clause per var (NULL if decision/axiom) */
    float  *activity;      /* VSIDS activity */
    unsigned char *seen;   /* scratch for conflict analysis */

    /* trail */
    lit  *trail;  int trail_sz, trail_cap;
    int  *trail_lim;       /* trail_lim[lvl] = trail index where lvl starts */
    int   decision_level;
    int   qhead;           /* propagation queue head */

    /* watch lists, indexed by literal (size 2*cap_vars) */
    vec_ptr *watches;

    /* VSIDS params */
    float var_inc, var_decay;

    /* clause activity + DB reduction */
    float cla_inc, cla_decay;
    clause **learnts; int nlearnts, cap_learnts;
    long    next_reduce;

    /* ORIGINAL (non-learnt) clauses, in insertion order. The watch lists are
     * the search structure but are unordered and shared; keeping this list:
     *   - lets sat_free() release them (they previously leaked: only learnts
     *     were freed, original clauses were reachable only through watches);
     *   - gives dimacs_write() / maxsat_dump_wcnf() a faithful dump of the
     *     problem as it was given to the solver, without learned clauses.
     * Original clauses are permanent in this engine (reduce_db only touches
     * learnts), so the pointers stay valid for the solver's lifetime. */
    clause **origs; int norigs, cap_origs;

    /* Permanent unit clauses (size-1 clauses). These must survive a reset to
     * the root state so that repeated solves (one per MaxSAT iteration) all
     * start from the same base of hard facts. */
    lit *units; int nunits, cap_units;

    /* stats */
    long conflicts, decisions, propagations, restarts;

    int ok;                /* 0 == trivially UNSAT (empty clause added) */

    /* ---- assumption support ---------------------------------------------
     * The core-guided MaxSAT loop needs to solve the formula *under a set of
     * assumptions* (temporary unit clauses that are NOT permanently added) and,
     * when the result is UNSAT, learn which assumptions participated in the
     * conflict. That subset is the "unsatisfiable core" and it is the signal
     * the MaxSAT reweighting loop uses to add optimality constraints.
     *
     * Mechanically an assumption is just a literal we enqueue as if it were a
     * decision, but we remember it so that on a root-level conflict we can walk
     * back through the reasons and report the assumptions that were actually
     * needed. */
    const lit *assumptions; /* current assumption literals (borrowed, not owned) */
    int   nassumptions;
    unsigned char *seen_assump; /* per-variable scratch during core extraction */
    int   assumptions_ok;   /* 0 => the assumptions themselves were trivially UNSAT */

    /* For each variable that is currently an "assumption selector", the
     * assumption literal it stands for. Indexed by variable; only meaningful
     * for variables where is_assump[v] != 0. This lets core extraction emit
     * the original assumption literals (correct polarity) rather than the
     * internal trail literals. */
    lit  *assump_lit;
    unsigned char *is_assump;
} solver;

typedef struct {
    long conflicts, decisions, propagations, restarts, learnts;
    /* VSIDS activity decay multiplier (1/(1+decay) is the "bump discount").
     * Reported by `planet --solver-stats` so users can see how aggressive
     * variable re-ranking is; 0 when no solve ran. */
    float vsids_decay;
} sat_stats;

/* ---- API --------------------------------------------------------------- */
solver *sat_new(void);
void    sat_free(solver *s);
var     sat_add_var(solver *s);
int     sat_nvars(const solver *s);
int     sat_ok(const solver *s);

/* Add a (non-learnt) clause. Returns 0 if trivially UNSAT, else 1.
 * lits may be unsorted / contain duplicates; tautologies are dropped. */
int  sat_add_clause(solver *s, lit *lits, int n);

/* Solve. Returns 1 == SAT, 0 == UNSAT. */
int  sat_solve(solver *s);

/* Solve under a set of temporary assumptions (literals). The assumptions are
 * treated as unit clauses for this call only; they are NOT permanently added
 * to the formula.
 *
 * Returns 1 == SAT (all assumptions are jointly satisfiable with the formula),
 *        0 == UNSAT.
 *
 * On UNSAT, if `core_out` is non-NULL it is filled with the subset of
 * `assumptions` that formed an unsatisfiable core, and `*core_n` is set to that
 * subset's size. This is the signal the core-guided MaxSAT loop consumes. */
int  sat_solve_assumptions(solver *s, const lit *assumptions, int n,
                           lit *core_out, int *core_n);

/* Return the solver to a clean root state (undo every assignment, re-enqueue
 * the permanent unit clauses). The MaxSAT layer calls this between solves when
 * it does NOT need to read a model, so it can extend the formula without the
 * new clauses colliding with a stale trail. */
void sat_reset(solver *s);

/* DEBUG ONLY: verify that every clause sits in exactly the watch lists of its
 * two watched literals (lits[0], lits[1]). Returns the number of stale entries
 * found and, when verbose != 0, prints each one to stderr. Used by the tests to
 * catch watch-list/literal-array divergence left behind by propagation. */
int  sat_debug_watch_check(solver *s, int verbose);

/* Truth value of a variable after a SAT solve. */
val  sat_val_var(const solver *s, var v);

void sat_stats_get(const solver *s, sat_stats *out);
void sat_print_stats(FILE *fp, const sat_stats *st);

#endif /* SAT_H */
