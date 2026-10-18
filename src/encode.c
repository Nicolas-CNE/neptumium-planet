/*
 * encode.c — packages -> weighted partial MaxSAT (see encode.h for the spec).
 * ==========================================================================
 *
 * Build order inside enc_solve():
 *    1. one free decision variable per candidate (repo entry)
 *    2. per package NAME: sequential-ladder AMO (at most one version) and the
 *       footprint+version soft family  (¬x_c) @ footprint + version*rank
 *    3. per candidate: Depends      -> (¬x_c ∨ providers...)   HARD
 *                      Conflicts    -> (¬x_c ∨ ¬x_other)       HARD
 *                      Replaces     -> (¬x_c ∨ ¬x_other)       HARD
 *    4. per user request: (some provider chosen)               SOFT @ W_REQUEST
 *    5. hand it all to the core-guided solver; read back the model.
 *
 * Every literal is built with lit_mk(var, sign): sign 0 = "variable true",
 * sign 1 = "variable false". Variables for candidates are allocated first so
 * the model read-back (maxsat_value) addresses them directly.
 */
#include "encode.h"
#include "maxsat.h"
#include "amo.h"

#include <stdlib.h>
#include <string.h>

/* ---- weight tiers -------------------------------------------------------- */

#define W_REQUEST_DEFAULT 10000L
#define W_VERSION_DEFAULT   100L
#define W_FOOTPRINT_DEFAULT    1L

enc_weights enc_default_weights(void) {
    enc_weights w = { W_REQUEST_DEFAULT, W_VERSION_DEFAULT, W_FOOTPRINT_DEFAULT };
    return w;
}

/* ---- data structures ----------------------------------------------------- */

/* One repo entry mapped to a decision variable. */
typedef struct {
    const package *p;   /* borrowed from the repo (repo outlives encoder) */
    int   group;        /* index of the name-group it belongs to           */
    var   v;            /* decision variable, assigned in enc_solve        */
} cand;

/* All versions that share one package NAME (candidates for AMO). */
typedef struct {
    const char *name;   /* borrowed from the first member's package        */
    int  *members;      /* candidate indices, sorted NEWEST first          */
    int   n, cap;
} ngroup;

/* A user request waiting to be encoded. */
typedef struct {
    char  *name;
    dep_op op;
    char  *ver;         /* NULL iff op == DEP_ANY */
    long   weight;      /* <= 0 means "use W_REQUEST tier" */
} request;

struct encoder {
    const repo *r;
    enc_weights w;

    cand    *cands;     int ncands;
    ngroup  *groups;    int ngroups;
    request *reqs;      int nreqs, cap_reqs;
    request *hreqs;     int nhreqs, cap_hreqs;   /* hard keep-pins (remove/upgrade) */

    maxsat *m;          /* built fresh on each enc_solve */

    int    *chosen;     /* per-candidate 0/1, valid after MAXSAT_OPTIMAL */
    long    cost;
    int     nsatisfied; /* requests met by the optimal set */
};

/* ---- small helpers ------------------------------------------------------- */

/* Index of the candidate whose package pointer is p, or -1.
 * repo_lookup hands back pointers straight from the repo, and cands[] mirrors
 * repo order, so pointer identity finds the matching decision variable. */
static int cand_index(const encoder *e, const package *p) {
    for (int i = 0; i < e->ncands; i++)
        if (e->cands[i].p == p) return i;
    return -1;
}

/* Insertion sort of a group's members NEWEST first (descending version).
 * Groups are tiny (versions of one package), so a simple sort beats pulling in
 * a context-carrying qsort comparator. */
static void sort_group_newest(encoder *e, ngroup *g) {
    for (int i = 1; i < g->n; i++) {
        int key = g->members[i];
        const char *kver = e->cands[key].p->version;
        int j = i - 1;
        while (j >= 0 && ver_cmp(e->cands[g->members[j]].p->version, kver) < 0) {
            g->members[j + 1] = g->members[j];
            j--;
        }
        g->members[j + 1] = key;
    }
}

static int group_push(ngroup *g, int idx) {
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 4;
        int *nm = realloc(g->members, (size_t)g->cap * sizeof *nm);
        if (!nm) return 0;
        g->members = nm;
    }
    g->members[g->n++] = idx;
    return 1;
}

/* ---- lifecycle ----------------------------------------------------------- */

encoder *enc_new(const repo *r) {
    encoder *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->r     = r;
    e->w     = enc_default_weights();

    int n = repo_count(r);
    e->cands = calloc((size_t)(n ? n : 1), sizeof *e->cands);
    if (!e->cands) { free(e); return NULL; }
    e->ncands = n;
    for (int i = 0; i < n; i++) {
        e->cands[i].p     = repo_get(r, i);
        e->cands[i].group = -1;
    }

    /* Group candidates by package NAME so each group can get one AMO and the
     * newest-first footprint/version soft family. */
    for (int i = 0; i < n; i++) {
        const package *p = e->cands[i].p;
        int g = -1;
        for (int j = 0; j < e->ngroups; j++)
            if (e->groups[j].name && strcmp(e->groups[j].name, p->name) == 0) {
                g = j; break;
            }
        if (g < 0) {
            ngroup *ng = realloc(e->groups, (size_t)(e->ngroups + 1) * sizeof *ng);
            if (!ng) { enc_free(e); return NULL; }
            e->groups = ng;
            g = e->ngroups++;
            e->groups[g].name    = p->name;
            e->groups[g].members = NULL;
            e->groups[g].n = e->groups[g].cap = 0;
        }
        if (!group_push(&e->groups[g], i)) { enc_free(e); return NULL; }
        e->cands[i].group = g;
    }
    for (int j = 0; j < e->ngroups; j++) sort_group_newest(e, &e->groups[j]);
    return e;
}

void enc_set_weights(encoder *e, enc_weights w) {
    if (e) e->w = w;
}

void enc_free(encoder *e) {
    if (!e) return;
    for (int j = 0; j < e->ngroups; j++) free(e->groups[j].members);
    free(e->groups);
    for (int k = 0; k < e->nreqs; k++) { free(e->reqs[k].name); free(e->reqs[k].ver); }
    free(e->reqs);
    for (int k = 0; k < e->nhreqs; k++) { free(e->hreqs[k].name); free(e->hreqs[k].ver); }
    free(e->hreqs);
    free(e->cands);
    free(e->chosen);
    if (e->m) maxsat_free(e->m);
    free(e);
}

/* ---- requests ------------------------------------------------------------ */

int enc_request(encoder *e, const char *name, dep_op op, const char *ver,
                long weight) {
    if (!e || !name || !*name) return 0;
    /* Nothing in the repo can ever satisfy it => the caller should report
     * "not found" rather than ask the solver to optimise the impossible. */
    if (!repo_has_provider(e->r, name, op, ver)) return 0;
    /* An unversioned constraint (or a bare name) is what DEP_ANY encodes. */
    if (op == DEP_ANY) ver = NULL;

    if (e->nreqs == e->cap_reqs) {
        e->cap_reqs = e->cap_reqs ? e->cap_reqs * 2 : 8;
        request *nr = realloc(e->reqs, (size_t)e->cap_reqs * sizeof *nr);
        if (!nr) return 0;
        e->reqs = nr;
    }
    request *rq = &e->reqs[e->nreqs];
    /* Allocate explicitly so we can unwind cleanly on partial failure. */
    rq->name = malloc(strlen(name) + 1);
    rq->ver  = ver ? malloc(strlen(ver) + 1) : NULL;
    if (!rq->name || (ver && !rq->ver)) { free(rq->name); free(rq->ver); return 0; }
    strcpy(rq->name, name);
    if (ver) strcpy(rq->ver, ver);
    rq->op     = op;
    rq->weight = weight;
    e->nreqs++;
    return 1;
}

/* Register a HARD requirement: "the final set MUST contain something providing
 * (name, op, ver)". Unlike enc_request (a soft wish the solver may break at a
 * cost), this is a hard clause — used by remove/upgrade to pin packages that
 * must stay installed. Returns 1 if satisfiable, 0 if the name is absent. */
int enc_require(encoder *e, const char *name, dep_op op, const char *ver) {
    if (!e || !name || !*name) return 0;
    if (!repo_has_provider(e->r, name, op, ver)) return 0;
    if (op == DEP_ANY) ver = NULL;

    if (e->nhreqs == e->cap_hreqs) {
        e->cap_hreqs = e->cap_hreqs ? e->cap_hreqs * 2 : 8;
        request *nr = realloc(e->hreqs, (size_t)e->cap_hreqs * sizeof *nr);
        if (!nr) return 0;
        e->hreqs = nr;
    }
    request *rq = &e->hreqs[e->nhreqs];
    rq->name = malloc(strlen(name) + 1);
    rq->ver  = ver ? malloc(strlen(ver) + 1) : NULL;
    if (!rq->name || (ver && !rq->ver)) { free(rq->name); free(rq->ver); return 0; }
    strcpy(rq->name, name);
    if (ver) strcpy(rq->ver, ver);
    rq->op     = op;
    rq->weight = 0;
    e->nhreqs++;
    return 1;
}

/* ---- clause builders ----------------------------------------------------- */

/* (¬x_c ∨ some-provider-chosen) — the Depends clause. If NO candidate can
 * provide the dependency, c is simply uninstallable: emit the unit (¬x_c). */
static void add_depends(encoder *e, int ci, const package **prov, int *ok) {
    const package *p = e->cands[ci].p;
    var xv = e->cands[ci].v;
    for (int d = 0; d < p->ndepends; d++) {
        const dep *dd = &p->depends[d];
        int np = repo_lookup(e->r, dd->name, dd->op, dd->version, prov, e->ncands);
        if (np == 0) {
            lit unit = lit_mk(xv, 1);            /* ¬x_c : never installable */
            if (!maxsat_hard(e->m, &unit, 1)) { *ok = 0; return; }
            continue;
        }
        lit *cl = malloc((size_t)(np + 1) * sizeof *cl);
        if (!cl) { *ok = 0; return; }
        cl[0] = lit_mk(xv, 1);                   /* ¬x_c */
        for (int t = 0; t < np; t++) {
            int j = cand_index(e, prov[t]);
            cl[1 + t] = lit_mk(j >= 0 ? e->cands[j].v : xv, 0);
        }
        if (!maxsat_hard(e->m, cl, np + 1)) *ok = 0;
        free(cl);
        if (!*ok) return;
    }
}

/* (¬x_c ∨ ¬x_other) for every candidate matching the relation — used for BOTH
 * Conflicts and Replaces, since each forbids the two from coexisting (see
 * encode.h). `self` is the candidate emitting the clause; a package that
 * conflicts with something only IT provides is skipped (would be a self-pin). */
static void add_noncoexist(encoder *e, int ci, const dep *d,
                           const package **prov, int *ok) {
    const package *self = e->cands[ci].p;
    var xv = e->cands[ci].v;
    int np = repo_lookup(e->r, d->name, d->op, d->version, prov, e->ncands);
    for (int t = 0; t < np; t++) {
        if (prov[t] == self) continue;           /* ignore self-overlap */
        int j = cand_index(e, prov[t]);
        if (j < 0) continue;
        lit cl[2] = { lit_mk(xv, 1), lit_mk(e->cands[j].v, 1) };
        if (!maxsat_hard(e->m, cl, 2)) { *ok = 0; return; }
    }
}

/* ---- the main build + solve ---------------------------------------------- */

/* Build the whole MaxSAT instance into e->m: decision variables, the AMO
 * ladder, the footprint/version soft family, the hard dependency and
 * non-coexistence clauses, and the request soft clauses. Returns 0 on
 * success, MAXSAT_UNSAT if allocation fails (the only "cannot build" case:
 * an uninstallable package is merely an unsatisfiable subproblem the solver
 * reports properly).
 *
 * Split out of enc_solve() so `planet dump-cnf` can export the exact instance
 * WITHOUT running the solver — invaluable for reproducing encoding bugs. */
static int enc_build(encoder *e) {
    if (!e) return MAXSAT_UNSAT;
    if (e->m) { maxsat_free(e->m); e->m = NULL; }
    free(e->chosen); e->chosen = NULL;
    e->cost = 0; e->nsatisfied = 0;

    e->m = maxsat_new();
    if (!e->m) return MAXSAT_UNSAT;

    /* 1. one decision variable per candidate */
    for (int i = 0; i < e->ncands; i++)
        e->cands[i].v = maxsat_var(e->m);

    solver *eng = maxsat_engine(e->m);
    const package **prov = malloc((size_t)(e->ncands ? e->ncands : 1) * sizeof *prov);
    if (!prov) return MAXSAT_UNSAT;
    int ok = 1;

    /* 2. per name: AMO + footprint/version soft family */
    for (int gi = 0; gi < e->ngroups && ok; gi++) {
        ngroup *g = &e->groups[gi];
        if (g->n >= 2) {
            lit *xs = malloc((size_t)g->n * sizeof *xs);
            if (!xs) { ok = 0; break; }
            for (int k = 0; k < g->n; k++)
                xs[k] = lit_mk(e->cands[g->members[k]].v, 0);
            amo_sequential_ladder(eng, xs, g->n);   /* HARD, linear clause count */
            free(xs);
        }
        /* members[] is NEWEST-first, so rank k = 0 is the newest candidate and
         * its (¬x) clause costs only the footprint base; older ranks add
         * version*k. Installing nothing costs 0. */
        for (int k = 0; k < g->n; k++) {
            lit nlit = lit_mk(e->cands[g->members[k]].v, 1);   /* ¬x_c */
            long wt = e->w.footprint + e->w.version * k;
            if (wt > 0) maxsat_soft(e->m, &nlit, 1, wt);
        }
    }

    /* 3. per-candidate Depends / Conflicts / Replaces */
    for (int i = 0; i < e->ncands && ok; i++) {
        const package *p = e->cands[i].p;
        add_depends(e, i, prov, &ok);
        if (!ok) break;
        for (int d = 0; d < p->nconflicts && ok; d++)
            add_noncoexist(e, i, &p->conflicts[d], prov, &ok);
        for (int d = 0; d < p->nreplaces && ok; d++)
            add_noncoexist(e, i, &p->replaces[d], prov, &ok);
    }

    /* 4. requests: soft (some provider chosen) */
    for (int k = 0; k < e->nreqs && ok; k++) {
        request *rq = &e->reqs[k];
        int np = repo_lookup(e->r, rq->name, rq->op, rq->ver, prov, e->ncands);
        if (np == 0) continue;                     /* already filtered out */
        lit *cl = malloc((size_t)np * sizeof *cl);
        if (!cl) { ok = 0; break; }
        for (int t = 0; t < np; t++) {
            int j = cand_index(e, prov[t]);
            cl[t] = lit_mk(j >= 0 ? e->cands[j].v : e->cands[0].v, 0);
        }
        long wt = rq->weight > 0 ? rq->weight : e->w.request;
        if (!maxsat_soft(e->m, cl, np, wt)) ok = 0;
        free(cl);
    }

    /* 4b. hard requirements: (x_p1 ∨ ... ∨ x_pn) — at least one provider MUST
     * be chosen. Emitted as a hard clause so the solver cannot drop a pin. */
    for (int k = 0; k < e->nhreqs && ok; k++) {
        request *rq = &e->hreqs[k];
        int np = repo_lookup(e->r, rq->name, rq->op, rq->ver, prov, e->ncands);
        if (np == 0) { ok = 0; break; }   /* should not happen: filtered above */
        lit *cl = malloc((size_t)np * sizeof *cl);
        if (!cl) { ok = 0; break; }
        for (int t = 0; t < np; t++) {
            int j = cand_index(e, prov[t]);
            cl[t] = lit_mk(j >= 0 ? e->cands[j].v : e->cands[0].v, 0);
        }
        if (!maxsat_hard(e->m, cl, np)) ok = 0;
        free(cl);
    }
    free(prov);
    return ok ? 0 : MAXSAT_UNSAT;
}

int enc_solve(encoder *e) {
    int rc = enc_build(e);
    if (rc != 0) return rc;

    /* 5. solve */
    rc = maxsat_solve(e->m);
    if (rc != MAXSAT_OPTIMAL) return rc;

    /* read back the model */
    e->chosen = calloc((size_t)(e->ncands ? e->ncands : 1), sizeof *e->chosen);
    if (!e->chosen) return MAXSAT_UNSAT;
    for (int i = 0; i < e->ncands; i++)
        e->chosen[i] = maxsat_value(e->m, e->cands[i].v);
    e->cost = maxsat_cost(e->m);

    /* count requests the model actually meets */
    for (int k = 0; k < e->nreqs; k++)
        if (enc_request_met(e, k)) e->nsatisfied++;
    return MAXSAT_OPTIMAL;
}

int enc_dump_cnf(encoder *e, FILE *fp) {
    /* Build the instance exactly like enc_solve() would, but stop before the
     * solver runs and export it as WCNF. Returns the clause count or -1. */
    if (!e || !fp) return -1;
    if (enc_build(e) != 0) return -1;
    return maxsat_dump_wcnf(e->m, fp);
}

/* ---- results ------------------------------------------------------------- */

int enc_ncandidates(const encoder *e)          { return e ? e->ncands : 0; }
const package *enc_candidate(const encoder *e, int i) {
    return (e && i >= 0 && i < e->ncands) ? e->cands[i].p : NULL;
}
int enc_chosen(const encoder *e, int i) {
    return (e && e->chosen && i >= 0 && i < e->ncands) ? e->chosen[i] : 0;
}
long enc_cost(const encoder *e)                { return e ? e->cost : 0; }
int  enc_satisfied(const encoder *e)           { return e ? e->nsatisfied : 0; }
long enc_rounds(const encoder *e) {
    return (e && e->m) ? maxsat_rounds(e->m) : 0;
}
void enc_stats(const encoder *e, sat_stats *out) {
    if (e && e->m) maxsat_stats(e->m, out);
    else if (out) memset(out, 0, sizeof *out);
}

int enc_request_met(const encoder *e, int k) {
    if (!e || !e->chosen || k < 0 || k >= e->nreqs) return 0;
    const request *rq = &e->reqs[k];
    const package **prov = malloc((size_t)(e->ncands ? e->ncands : 1) * sizeof *prov);
    if (!prov) return 0;
    int met = 0;
    int np = repo_lookup(e->r, rq->name, rq->op, rq->ver, prov, e->ncands);
    for (int t = 0; t < np && !met; t++) {
        int j = cand_index(e, prov[t]);
        if (j >= 0 && e->chosen[j]) met = 1;
    }
    free(prov);
    return met;
}
