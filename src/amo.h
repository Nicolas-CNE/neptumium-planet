#ifndef AMO_H
#define AMO_H
/* At-Most-One (AMO) constraint encodings.
 *
 * Planet needs AMO in two places:
 *   - per-package "choose at most one version" over multi-version repos
 *   - solver-internal cardinality constraints in the MaxSAT core-guided loop
 *
 * We use the *sequential ladder* encoding so the clause count stays O(N)
 * (naive pairwise is O(N^2), which blows up on repos with many versions).
 */
#include "sat.h"

/* Sequential-ladder AMO over the n literals in xs.
 * Introduces (n-1) auxiliary variables and emits 2n-2 clauses.
 * Returns the number of auxiliary variables created. */
int amo_sequential_ladder(solver *s, const lit *xs, int n);

/* Pairwise AMO (O(N^2) clauses, no aux vars). Kept for small n and for
 * cross-checking the ladder encoding in tests. */
int amo_pairwise(solver *s, const lit *xs, int n);

#endif /* AMO_H */
