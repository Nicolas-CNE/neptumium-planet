#define _GNU_SOURCE
#include "dimacs.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Parse a DIMACS CNF from `fp` into the solver.
 *
 * Handles the standard grammar:
 *   c <comment>
 *   p cnf <nvars> <nclauses>
 *   <lit> <lit> ... 0        (one clause per run of literals ending in 0)
 *
 * Variables are created on demand up to the header's declared count so that
 * the solver's var space matches the file even if some vars are unused.
 * Returns 1 on success, 0 on parse error.
 */
int dimacs_read(solver *s, FILE *fp) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int seen_header = 0;
    int err = 0;

    lit  *buf = NULL;
    int   buf_cap = 0, buf_n = 0;   /* pending clause literals */

    while (!err && (len = getline(&line, &cap, fp)) != -1) {
        /* strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == 'c' || *p == '\0' || *p == '\n') continue;

        if (*p == 'p') {
            /* header: p cnf <nvars> <nclauses> */
            int nv, nc;
            if (sscanf(p, "p cnf %d %d", &nv, &nc) != 2) { err = 1; break; }
            for (int i = 0; i < nv; i++) sat_add_var(s);
            seen_header = 1;
            continue;
        }
        if (!seen_header) { err = 1; break; }   /* clause before header */

        /* parse a run of integer literals on this line */
        char *tok = strtok(p, " \t\r\n");
        while (tok) {
            char *end;
            long v = strtol(tok, &end, 10);
            if (end == tok) { err = 1; break; }   /* not a number */
            if (v == 0) {
                /* end of clause: flush */
                if (buf_n > 0) {
                    /* A 0 return means the formula became unsatisfiable
                     * (e.g. a conflicting unit clause) — that's a legitimate
                     * outcome, not a parse error. Keep parsing; sat_solve()
                     * will report UNSAT via the solver's ok flag. */
                    (void)sat_add_clause(s, buf, buf_n);
                    buf_n = 0;
                }
            } else {
                if (buf_n == buf_cap) {
                    buf_cap = buf_cap ? buf_cap * 2 : 16;
                    buf = realloc(buf, (size_t)buf_cap * sizeof *buf);
                    if (!buf) { err = 1; break; }
                }
                var vv = (var)(v < 0 ? -v : v) - 1;   /* DIMACS is 1-based */
                if (vv >= sat_nvars(s)) { err = 1; break; }
                buf[buf_n++] = lit_mk(vv, v < 0);
            }
            tok = strtok(NULL, " \t\r\n");
        }
    }

    if (buf_n > 0 && !err) { (void)sat_add_clause(s, buf, buf_n); }
    free(buf);
    free(line);
    return !err;
}

/* Write the solver's variable space + learnt clause set as DIMACS CNF.
 * (For `planet dump-cnf`; we emit learnt clauses so an external solver can
 * cross-check the exact state Planet reached.) */
int dimacs_write(const solver *s, FILE *fp) {
    /* Dump the problem AS GIVEN to the solver: the permanent unit clauses plus
     * every original (non-learnt) clause, in insertion order. Learnt clauses
     * are excluded: they are implied by the original problem and would only
     * inflate the file. */
    int nlive = 0;
    for (int i = 0; i < s->norigs; i++)
        if (!s->origs[i]->dead) nlive++;
    int nclauses = s->nunits + nlive;
    fprintf(fp, "c planet dump-cnf: %d vars, %d clauses "
                "(%d units + %d originals; learnts excluded)\n",
            sat_nvars(s), nclauses, s->nunits, nlive);
    fprintf(fp, "p cnf %d %d\n", sat_nvars(s), nclauses);

    for (int i = 0; i < s->nunits; i++) {
        lit l = s->units[i];
        fprintf(fp, "%s%d 0\n", lit_sign(l) ? "-" : "", lit_var(l) + 1);
    }
    for (int i = 0; i < s->norigs; i++) {
        clause *c = s->origs[i];
        if (c->dead) continue;
        for (int k = 0; k < c->size; k++) {
            lit l = c->lits[k];
            fprintf(fp, "%s%d ", lit_sign(l) ? "-" : "", lit_var(l) + 1);
        }
        fprintf(fp, "0\n");
    }
    return nclauses;
}
