#ifndef DIMACS_H
#define DIMACS_H
/* Minimal DIMACS CNF reader/writer.
 * Used by `planet solve <cnf>` and `planet dump-cnf`. */
#include "sat.h"

/* Parse a DIMACS CNF from `fp` into the solver. Variables are created on
 * demand up to the header's declared count. Returns 0 on parse error. */
int dimacs_read(solver *s, FILE *fp);

/* Write the solver's current (non-learnt) clause set as DIMACS CNF.
 * Returns number of clauses written. */
int dimacs_write(const solver *s, FILE *fp);

#endif /* DIMACS_H */
