# Pathology library

Worked examples of numerical mistakes. Each file shows the broken
computation, the fix, and a paragraph stating the lesson. Run any
program with:

```
./knot --no-hints examples/pathologies/01_quadratic_cancellation.knot
```

The `--no-hints` flag suppresses the `take`-phrase translation
hints; drop it if you want to see how each English phrase desugars.

| # | File                              | Pathology                                                     |
|---|-----------------------------------|---------------------------------------------------------------|
| 1 | `01_quadratic_cancellation.knot`  | Catastrophic cancellation in the quadratic formula            |
| 2 | `02_naive_variance.knot`          | Naive variance E[X²] - E[X]² vs Welford's online algorithm    |
| 3 | `03_naive_sum.knot`               | Naive summation vs Kahan compensated summation                |
| 4 | `04_stiff_euler.knot`             | Explicit Euler diverges on a stiff ODE; implicit is stable    |
| 5 | `05_hilbert_solve.knot`           | Hilbert matrix linear solve is hopelessly ill-conditioned     |

## Execution-mode parity

Pathologies 01-04 produce identical output under `--interp` and
`--exec` (the transpiled C backend). The smoke test verifies this.

Pathology 05 does *not* produce identical output, because the
Hilbert matrix amplifies any ulp-level difference in intermediate
arithmetic and the two backends round subexpressions slightly
differently. This is itself a demonstration of the pathology --
the lesson is unchanged, only the trailing digits of the displayed
error differ.

## Candidates for v2

These were considered for v1 but cut for time. Each is a small file
on the same template:

- Newton-Raphson cycling on `f(x) = x^3 - 2x + 2`
- Float equality (`0.1 + 0.2 != 0.3`) and how to write tolerant checks
- Naive Gram-Schmidt loss of orthogonality vs modified Gram-Schmidt
- `1 - cos(x)` near zero (use `2*sin(x/2)^2` instead)
- Forward-substitution accumulating error vs backward
- The naive sample correlation formula
