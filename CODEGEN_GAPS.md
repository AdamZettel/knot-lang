# Codegen gaps

Things the `--exec` (transpile-to-C) backend doesn't yet lower from
knot source, with workarounds and rough fix sketches. The
interpreter (`--interp`, the default) supports all of these; the
gap is purely on the codegen side.

Knot's headline invariant is that `--interp` and `--exec` produce
byte-identical output. The pathology library and stress tests
parity-check this where it holds; this file is the running list
of where it currently doesn't.

Entries are ordered by **priority**:
- **high** — blocks shipping common idiomatic code in `--exec`
- **medium** — narrower impact, real but easier to work around
- **low** — minor, mostly cosmetic

## high

### Heterogeneous list types

**Surfaced by:** any code that returns multiple values from a function
by packing them into a list, e.g.
`def eig_sym_2x2(M) { ...; return [lam1, lam2, U] }` — the eigenvalue
solver in the electronic-structure thread originally took this shape.

**What happens:** the codegen emits `cannot index double` because it
doesn't infer a list-of-mixed-types return; the function is typed as
returning a num.

**Workaround:** out-parameters. Allocate the result containers in the
caller and pass them in for the function to fill (LAPACK convention).

**Fix sketch:** real tuple support would need a new CType, tuple-of-T
plumbing through `c_decl`, calling convention, return handling. Or:
support knot-list-of-num-and-mat via a tagged-union runtime value, but
that's a much larger lift. ~300 LOC for tuples, more for tagged unions.
The workaround is good enough for v1; the LAPACK-style out-parameter
shape is idiomatic in numerics anyway.

### Hadamard `*` and `/` between two vecs / mats

**Surfaced by:** `examples/stress/04_linreg.knot` (worked around
with a manual loop), `tests/broadcast.knot` (interp-only blocks),
any code computing `sum(x*x)` or similar elementwise products.

**What happens:**

    error: codegen: * undefined for these types

The interpreter handles this in `apply_binop` via `vec_emul` /
`mat_emul`; the codegen path has no equivalent.

**Workaround:** explicit elementwise loop with `at` / `set`. Adds
visible loop noise but lowers cleanly.

**Fix sketch:** add `knot_vec_emul` / `knot_vec_ediv` /
`knot_mat_emul` / `knot_mat_ediv` to `src/runtime.h` mirroring
the existing `knot_vec_add` etc., then wire `emit_binary` in
`src/codegen.hpp` to call them for vec/vec and mat/mat operands
with `*` and `/`. ~80 LOC total.

### Broadcast `+` / `-` between scalar and vec/mat

**Surfaced by:** any code like `v - mu` or `1.0 / x` where `x` is
a vec.

**What happens:**

    error: codegen: + undefined for these types

**Workaround:** explicit loop, or `vec_scale(v, -1)` + `vec_add`
for some cases.

**Fix sketch:** runtime helpers `knot_vec_add_scalar` /
`knot_scalar_sub_vec` / `knot_scalar_div_vec` etc. and the
matching `emit_binary` branches. Same shape as the Hadamard
fix; ~60 LOC.

### `format(fmt, ...args)` builtin

**Surfaced by:** the original `examples/pathologies/03_naive_sum`
(reworked to use `show` instead), `examples/playground` Z-score
example, basic narrate-with-numbers in tests.

**What happens:**

    error: codegen: unknown function: format

**Workaround:** use `show EXPR` (which IS lowered) for one-off
labeled output. For multi-value lines, multiple `show` statements
in sequence. For really custom formatting, stay in `--interp`.

**Fix sketch:** add `knot_format(const char* fmt, ...)` to
`src/runtime_ext.cpp` reusing the same parse logic as `b_format`
in `src/interpreter.hpp`. Codegen treats `format` as a special
case in `emit_call` because of the variadic / typed-arg dispatch.
~120 LOC plus runtime wiring.

## medium

### `str(value)` builtin

**Surfaced by:** anywhere a string-of-anything is needed; e.g.
`narrate "x = " + str(x)`.

**What happens:**

    error: codegen: unknown function: str

**Workaround:** for scalars, use `show x` (gets a `x: value` line
to stdout). For dynamic message construction, stay in `--interp`.

**Fix sketch:** runtime helper `knot_to_string(double, char*
buf)` and the type-dispatched variants. Mostly mechanical; the
hard part is allocating the result string. Could keep a
process-static buffer in the runtime for v1 (single-use; not
reentrant). ~70 LOC.

### `plot(x, y[, name])` and `plot_save(path, x, y_or_Y[, name])` builtins

**Surfaced by:** the playground's plot examples when run under
`--exec`; `examples/plotting_demo.knot` under `--exec`.

**What happens:**

    error: codegen: unknown function: plot
    error: codegen: unknown function: plot_save

**Workaround:** plotting is a `--interp` feature today (browser
playground for `plot()`, JSON-file + `web/viewer.html` for
`plot_save()`). Run plotting programs through `--interp`.

**Fix sketch:** add `knot_plot_v`, `knot_plot_m`, `knot_plot_save_v`,
`knot_plot_save_m` runtime helpers that emit the same `__knot_plot__`
stdout line / JSON file the interpreter does. Once `--exec` knows how
to call them, both builtins work the same in both backends. ~80 LOC.

### `sin(vec)` / `cos(vec)` / etc. broadcasting

**Surfaced by:** the playground's `sin/cos` plot example,
naturally written `y = sin(x)` where `x` is a vec.

**What happens:** in interp these return a vec via
`apply_unary_broadcast`. In codegen they're lowered as scalar
operations and the type-check fails before that.

**Workaround:** explicit loop, or pre-convert to a scalar context.

**Fix sketch:** in `src/codegen.hpp` `emit_call`, detect the
unary-math builtins by name and dispatch on operand type --
scalar -> single `knot_sin` call, vec -> a generated for-loop
that fills an output vec. Same pattern for the binary `pow` /
`atan2` if/when those need it. ~90 LOC.

## non-issues

These look like gaps but aren't:

- `take` phrases lowered to non-closure calls (`take the sum of
  v`, `take the projection of u onto v`, etc.) -- the desugared
  form is a plain Call, which lowers as long as the called
  function itself does.
- `show EXPR` and `show EXPR, EXPR, ...` -- lowered via per-arg
  `fputs("LABEL: ", stdout)` + `knot_print_*`.
- `narrate EXPR` (string-valued) -- lowered with a runtime
  `knot_narration_on` flag guard. Works in --exec; the
  --no-narrate flag flip is interp-only for now.
- `iterate over / iterate from-to / repeat times` -- these
  desugar at parse time to existing `StmtKind::For` nodes, so
  they go through the standard for-loop codegen with no
  modification.
- `fuzz` mode -- the runner is interp-only by design (the fuzzer
  loads stdlib + the program N times in fresh interpreters; this
  is a CLI feature, not a codegen one).
