# Stress tests

Real numerical programs that each verify a known answer with
`assert_near`. Together they exercise the language across the major
feature groups (closures, linear algebra, iteration, special
functions, regression) and surface remaining `--exec` codegen gaps
in concrete contexts.

Run any one as a test:
```
./knot --test examples/stress/01_gauss_legendre.knot
```

Or all five via the make target:
```
make test
```

| #  | File                          | Verifies                                                                 |
|----|-------------------------------|--------------------------------------------------------------------------|
| 01 | `01_gauss_legendre.knot`      | Bonnet recurrence + Newton + weight formula; integrates `x^4` exactly    |
| 02 | `02_eigsym_hilbert.knot`      | Power iteration on `H_5`; largest eigenvalue matches Wilkinson reference |
| 03 | `03_slater_norm.knot`         | Simpson on `r^2 exp(-2 zeta r)`; matches `pi / zeta^3`                   |
| 04 | `04_linreg.knot`              | Closed-form OLS recovers exact slope/intercept on noiseless data         |
| 05 | `05_power_iter.knot`          | Dominant eigenpair of a 3x3 symmetric matrix; matches `(7+sqrt 5)/2`     |

## Execution-mode coverage

Each program runs cleanly under `--interp`. `--exec` parity holds
for 01, 02, 04, 05; the smoke runner checks parity on those four.

`03_slater_norm` passes an `fn(r) -> EXPR` closure into `simpson()`.
The transpiler doesn't lower `FnExpr` yet (see `CODEGEN_GAPS.md`), so
this file is interp-only. The smoke runner only sanity-checks it
under `--interp`.

## Candidates for v2

Programs 6-11 in the original plan, deferred until the closure ABI
lands in `--exec` and the broadcast / vec arithmetic gaps close:

- `06_simpson_demo.knot`           composite Simpson with `exp(-x^2) cos x`
- `07_runge_phenomenon.knot`       polynomial interpolation on equispaced nodes
- `08_scf_two_level.knot`          self-consistent-field on a 2-level model
- `09_lanczos.knot`                Lanczos diagonalization of a tight-binding chain
- `10_rk4_harmonic.knot`           rk4 on a vec-valued ODE
- `11_h2plus.knot`                 H₂⁺ in a two-Gaussian basis (the headline result)
