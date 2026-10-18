#include "amo.h"
#include <stdlib.h>

static void add2(solver *s, lit a, lit b) {
    lit buf[2] = { a, b };
    sat_add_clause(s, buf, 2);
}

int amo_pairwise(solver *s, const lit *xs, int n) {
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            add2(s, lit_neg(xs[i]), lit_neg(xs[j]));
    return 0;
}

int amo_sequential_ladder(solver *s, const lit *xs, int n) {
    if (n <= 1) return 0;

    /* n == 2 is optimal as a single clause, no aux vars. */
    if (n == 2) {
        add2(s, lit_neg(xs[0]), lit_neg(xs[1]));
        return 0;
    }

    /* n >= 3: sequential ladder. a[i] means "some x[j] true for j <= i".
     * n-1 auxiliary variables a[0..n-2], 3n-4 clauses. */
    lit *a = (lit *)malloc((size_t)(n - 1) * sizeof(lit));
    for (int i = 0; i < n - 1; i++)
        a[i] = lit_mk(sat_add_var(s), 0);

    /* head: (¬x0 ∨ a0) */
    add2(s, lit_neg(xs[0]), a[0]);

    /* body: for i = 1..n-2 */
    for (int i = 1; i <= n - 2; i++) {
        add2(s, lit_neg(xs[i]),   a[i]);      /* ¬xi -> ai     */
        add2(s, lit_neg(a[i-1]),  a[i]);      /* a(i-1) -> ai  */
        add2(s, lit_neg(xs[i]),   lit_neg(a[i-1])); /* ¬xi ∨ ¬a(i-1) */
    }

    /* tail: (¬x(n-1) ∨ ¬a(n-2)) */
    add2(s, lit_neg(xs[n-1]), lit_neg(a[n-2]));

    free(a);
    return n - 1;
}
