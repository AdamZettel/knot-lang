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

### `fn(x) -> EXPR` (FnExpr)

**Surfaced by:** `examples/stress/03_slater_norm.knot`,
`tests/fn_expr.knot`, any program using `take the integral of EXPR
from A to B`, `take the root of EXPR starting at INIT`, or any
closure passed to a higher-order stdlib function.

**What happens:** the codegen emits

    error: codegen: anonymous `fn(x) -> EXPR` not supported in
    --exec yet; run this program with --interp

at parse time. The program is rejected before any C is generated.

**Workaround:** define the inner function as a top-level `def
NAME(x) { return EXPR }` and pass `NAME` instead of `fn(x) ->
EXPR`. Sidesteps the closure-capture entirely, since a top-level
def has no surrounding environment to capture.

**Fix sketch (Phase 9 in the plan):** introduce a fat-pointer
closure ABI in the runtime. Each callable value is a `{fn ptr,
env ptr}` pair. FnExpr compiles to a generated C function taking
an `env*` plus the original parameters; the env struct is
synthesized at codegen time from the free variables. Bare
top-level defs wrap with a thunk that ignores env. Cost: ~250 LOC
in `src/codegen.hpp` plus runtime helpers; the existing
`FnD_D / FnDD_D / FnDDD_D` types in `src/codegen.hpp` are the
starting point.

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

### `plot(x, y[, name])` builtin

**Surfaced by:** the playground's plot examples when run under
`--exec`.

**What happens:**

    error: codegen: unknown function: plot

**Workaround:** plotting is a `--interp` / browser-playground
feature today. Run `--exec` programs that need to plot through
`--interp` instead; the plot data renders identically.

**Fix sketch:** add `knot_plot_v` / `knot_plot_m` runtime helpers
that write the `__knot_plot__ {...}` sentinel-prefixed JSON line
to stdout. Once `--exec` knows how to call them, plot() works
the same in both backends. ~40 LOC.

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

## low

### `take`-phrase closure forms in `--exec`

**Surfaced by:** any `take the integral of EXPR from A to B`,
`take the root of EXPR starting at INIT` under `--exec`.

**What happens:** the phrase desugars to a Call with a `fn(x) ->
EXPR` callee, which hits the FnExpr gap above. The diagnostic
text is the FnExpr one, not phrase-specific.

**Workaround:** rewrite as a direct `simpson(named_fn, a, b)` or
`bisect(named_fn, a, b)` call with a top-level `def` for the
integrand.

**Fix sketch:** automatic once FnExpr lowers. No phrase-specific
work needed.

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
