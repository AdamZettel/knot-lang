# knot

A small numerical scripting language. Two execution modes from the same source:
**interpreted** for fast iteration (3ms startup), or **transpiled to C** for
native-binary speed (within ~10% of hand-written C, 40x faster than Python).

Built in ~3000 lines of C++17. Single binary. Builds with any C++17
compiler; `--exec` mode additionally shells out to a C compiler (`cc`)
on `$PATH`.

## Getting started in 60 seconds

Clone, build, run an example:

```bash
git clone https://github.com/AdamZettel/knot-lang.git
cd knot-lang
make
./knot examples/option_pricer.knot             # interpreted (~1s)
./knot --exec examples/option_pricer.knot      # transpile to C, run binary
```

Requires `g++` (or `clang++`) for C++17 and a C compiler (`cc`) on `$PATH`.
Both are present by default on Linux and on macOS with Xcode command-line
tools installed. No other dependencies.

The option pricer demos all three execution paths: Black-Scholes closed form,
1000-path Monte Carlo, and a Cox-Ross-Rubinstein binomial tree.  Interpreted
finishes in under a second. With `--exec` and the MC turned up to 100k paths,
the full thing (compile + run) takes about half a second; the cached binary
alone runs in under 10ms.

Other examples worth a look:

- `examples/tour.knot`            — basics: assignment, arithmetic, vec/mat ops, control flow
- `examples/for_tour.knot`        — the `for` construct + tagged-index bug detection
- `examples/bisect_annotated.knot` — paired with `.annot` to demo the annotation system

Try the annotated example with:

```bash
./knot --show examples/bisect_annotated.knot   # read source with annotations interleaved
./knot --step examples/bisect_annotated.knot   # narrated execution
```

## Modes

| Command | What it does |
|---|---|
| `./knot FILE` | Run FILE in the interpreter. ~3ms startup. |
| `./knot --exec FILE` | Transpile FILE to C, compile with `cc -O2`, run. ~130ms cold, ~4ms warm (cache). |
| `./knot --cc FILE` | Print the generated C to stdout. |
| `./knot --show FILE` | Print source with sidecar `.annot` annotations interleaved as comments. |
| `./knot --step FILE` | Run interpreted with annotations printed as commentary. |
| `./knot --scaffold FILE` | Emit a `.annot` template you fill in by hand. |
| `./knot` | Start the REPL. Try `lslib` and `whatis solve`. |

## Surface syntax

```python
# Bare assignment creates or rebinds (no `let`).
x = 3
y = 4
greet = "hello, " + "world"

# Vectors and matrices (column-major internally; written row-by-row).
v = [1, 2, 3, 4]
w = [5, 6, 7, 8]
A = [[1, 2], [3, 4]]
B = [[5, 6], [7, 8]]

# Arithmetic.  `*` is scalar/elementwise.  `@` is matrix product.
v + w, 2 * v, v / 3
A + B, 2 * A
A @ B            # matrix-matrix product
A @ v            # matrix-vector product
v @ A            # vec-as-row times mat
v @ w            # dot product (returns a number)

# Indexing with negative indices (Python-style).
v[2], v[-1]
A[0, 1], A[-1, -1]

# Slicing (returns a copy).
v[1:3], v[:2], v[2:], v[-2:]
"hello"[1:4]     # "ell"

# Mutation, including compound assignment.
v[0] = 99
v[-1] = 0
i += 1
x *= 2

# Control flow.  Parens around the condition are optional.
if x > 0 {
    print("positive")
}
else if x == 0 {
    print("zero")
}
else {
    print("negative")
}
while i < 10 {
    sum += i
    i += 1
}

# Loops -- four explicit forms, no off-by-one bugs.
for i to n             { ... }   # i = 0..n-1
for x in v             { ... }   # x walks the elements of v
for i to len(v)        { ... }   # index-only (when you need positions)
for i, x in v          { ... }   # both index and element

# `break` exits the nearest enclosing loop; `continue` jumps to its next
# iteration. Both error out if used outside a loop, including across a
# function-call boundary.
for i to 1000 {
    if found(i) { break }
}

# Functions with default arguments.  Closures work.
def power_iter(M, iters=50, tol=1e-10) {
    "Dominant eigenvalue by power iteration."   # first string = docstring
    n = rows(M)
    x = ones(n)
    lam = 0
    for _ to iters {
        x = M @ x
        new_lam = norm(x)
        x /= new_lam
        if abs(new_lam - lam) < tol { return new_lam }
        lam = new_lam
    }
    return lam
}

# Lists (heterogeneous, indexable, appendable).  Vec literals become lists
# when elements aren't all numeric, or when empty.
result = [M, perm]           # list of mat + vec
M_returned = result[0]

xs = []                      # empty list
append(xs, 1)
append(xs, "two")            # mixed types fine -- it's a list, not a vec
print(len(xs), xs[0], xs[1]) # 2 1 two

# Statement terminators: newline or `;`.  Inside `(` `)` and `[` `]`,
# newlines are continuation whitespace, so multi-line literals work.
M = [[1, 2, 3],
     [4, 5, 6],
     [7, 8, 9]]
```

## Shape-tagged indices

Every matrix carries axis tags.  `rows(A)`, `cols(A)`, and `len(v)` return
numbers tagged with their provenance, and that tag propagates through
arithmetic (`i - 1`, `2*j`) and through `for` index variables.  When you
later index a vec or mat with a tagged number, the tag is checked against
the axis being indexed.  This catches several real bug classes:

```python
A = [[1, 2, 3], [4, 5, 6]]
for i to rows(A) {
    for j to cols(A) {
        print(A[j, i])   # ERROR: index came from cols(mat) but is used as row index
    }
}
```

The check fires even when the matrix is square -- this is the case bounds-
checking alone misses, because the values happen to be valid.  It also fires
when you use a tagged index from one matrix on a *different* matrix, even
if their shapes happen to match.

### "But what if I actually want the transpose?"

The flagged example above is itself a half-finished transpose-print: someone
who wanted to print `A^T` and tried to do it by swapping the letters in the
inner expression.  That's exactly the bug the system is meant to catch.  To
print the transpose, swap the *loop order* instead of the index roles:

```python
# print A^T: outer loop = which row of A^T (= column of A)
for i to cols(A) {
    for j to rows(A) {
        print(A[j, i])   # rows-tagged j as row index, cols-tagged i as col index -- OK
    }
}
```

The two snippets look almost identical, which is the point: the tag check is
what tells them apart.  If you genuinely want to allocate a transposed copy,
`B = transpose(A)` gives you a fresh matrix with its own tags and you iterate
it the obvious way.

### Opting out

The tag is permissive when *either* side is untagged.  Use the `at(c, i)` /
`set(c, i, v)` / `at(M, i, j)` / `set(M, i, j, v)` builtins for untagged
access -- handy in generic library code where the caller's contract is that
lengths match (e.g. `solve(A, b)` reading `b[perm[i]]`).

## Standard library

Loaded automatically into globals.  Use `lslib` in the REPL to list it,
`whatis NAME` for the full docstring.

Array utilities: `sum`, `prod`, `mean`, `vmin`, `vmax`, `argmin`, `argmax`,
`linspace`, `arange`, `reverse`, `fill`, `copy_vec`, `copy_mat`

Statistics: `variance`, `std`, `median`, `floor_div`

Sorting: `sort` (insertion-sort, O(n²); for any non-trivial n use the
`sort_vec` builtin from the runtime extensions instead)

Linear algebra: `trace`, `diag`, `lu` (returns `[LU, perm]`), `solve`,
`power_iter` (returns `[lambda, eigvec]`)

Root finding: `bisect`, `newton`

Quadrature: `trapezoid`, `simpson`

ODE: `rk4` (interpreter only — passes a function as an argument, which the
transpiler doesn't yet handle)

Optimization: `golden_section`

## Builtins (in the core, not the stdlib)

Numbers: `sqrt`, `abs`, `sin`, `cos`, `exp`, `log`
IO/convert: `print`, `input`, `num`, `str`, `len`
Arrays: `zeros(n) | zeros(r,c)`, `ones(n) | ones(r,c)`, `eye(n)`,
        `rows(M)`, `cols(M)`, `dot(v, w)`, `norm(v)`,
        `matmul(a, b)` (or use `@`), `transpose(M)`
Untagged access: `at(c, i)`, `at(M, i, j)`, `set(c, i, v)`, `set(M, i, j, v)`
Lists: `append(list, value)` (interpreter only — the transpiler doesn't
       handle heterogeneous lists yet)

Runtime extensions (compiled C++, available in both interpreter and
`--exec`):
- `sort_vec(v)` — in-place quicksort, the fast path for sorting
- `rng_seed(n)`, `rng_uniform()`, `rng_normal()` — deterministic RNG
  (same seed gives same sequence in both execution modes)
- `read_csv(path)` returns a mat; `write_csv(M, path)` is its inverse

Operators also work as words: `and`, `or`, `not` alongside `&&`, `||`, `!`.

## Reading order (for studying)

1. `span.hpp` — `Span(start, length, line, col)`. Every token, AST node,
   and error carries one.
2. `token.hpp` — `Tok` enum and `Token` struct.
3. `lexer.hpp` — hand-written. Emits `Newline` tokens, suppressed inside
   `(...)` / `[...]` for implicit line continuation. Keywords map to enum
   variants. Word-operators (`and`/`or`/`not`) lex to the same tokens as
   their symbol forms.
4. `ast.hpp` — `Expr` and `Stmt` nodes as tagged structs. Worth scanning
   the kind enums first to see the surface.
5. `parser.hpp` — recursive descent with Pratt-style precedence climbing.
   Precedence ladder (low → high): `or`, `and`, `not`, `== !=`,
   `< <= > >=`, `+ -`, `* / % @`, unary, postfix `f(args)` / `a[idx]`,
   primary. Slices live in the postfix index path.
6. `linalg.hpp` — `Vec` (1-D) and `Mat` (column-major: `data[i + j*rows]`).
   Plain `std::vector<double>` storage; all ops throw `runtime_error` on
   shape mismatch and the interpreter wraps these with spans.
7. `value.hpp` — `Value` is `std::variant<None, Num, Bool, Str,
   shared_ptr<Vec>, shared_ptr<Mat>, shared_ptr<ValueList>,
   shared_ptr<Function>, Builtin>`.  `ValueList` is the heterogeneous
   list (return tuples, `append`-grown collections).  `Env` is a chained
   name→Value map; the parent pointer gives closures their captured
   scope.
8. `interpreter.hpp` — `exec(Stmt)` and `eval(Expr)`, one case per kind.
   `apply_binop` is factored out so compound assignment reuses it.
   `eval_index` handles negative indices and slices.  Builtins are
   registered at the bottom.
9. `render.hpp` — rustc-style error printer (caret under the source).
10. `main.cpp` — file driver and REPL.
11. `hash.hpp` — FNV-style content hash over AST shape.  Drives the
    annotation system (line 11 below) and the transpile cache (so an
    unchanged source skips the `cc` compile step).
12. `annotations.hpp` — `.annot` sidecar loader and lookup, keyed by
    AST content hashes, with composite-key (consecutive-stmt) support.
13. `codegen.hpp` — the transpiler.  AST → C strings with per-expression
    type inference (Num/Vec/Mat/FnD_D).  Read the comment at the top
    first for the authoritative list of what's not supported in v1.
14. `runtime.h` — the C runtime header the generated code links against.
    Vec/Mat helpers, formatting, error abort.
15. `runtime_ext.cpp` — sort/RNG/CSV implementations shared between the
    interpreter and the transpiled output.

## Annotations (pseudocode commentary on hashes)

Annotations are kept out-of-band, in a sidecar `.annot` file paired with
the source file. They're keyed by content hashes of the AST, so they
survive whitespace/comment edits but become orphaned (need updating) when
you change the *meaning* of a statement. This is the intended behavior: a
comment describing computation X should not silently re-attach to a
computation Y that replaced it.

Workflow (you should never have to touch a hash by hand):

```
$ ./knot --scaffold path/to/file.knot > path/to/file.knot.annot
$ # edit the file.knot.annot, replacing each "TODO: ..." with a real description
$ ./knot --show path/to/file.knot      # read your annotated source
$ ./knot --step path/to/file.knot      # run with annotations interleaved
```

Re-scaffolding after editing the source keeps your existing descriptions
(including composite groupings) and only inserts `TODO:` lines for
statements that don't have entries yet.

File format (deliberately boring; LLM-friendly):

    # comments allowed
    HASH<TAB>description
    HASH1+HASH2+...<TAB>description for a run of consecutive stmts
    HASH<TAB>*verbose description (prints every iteration)

Composite keys group consecutive statements at the same scope under a
single annotation, useful for "these three lines compute the gradient"
without commenting each one. The runtime prefers the longest composite
match starting at each statement.

By default each annotation prints at most once per run, so a tight loop
doesn't flood the output. Prefix a description with `*` to opt back into
"print every iteration."

When `--step` runs, annotations stream to stderr (so they don't interfere
with the program's stdout). See `examples/bisect_annotated.knot` and its
`.annot` file for a worked example.

## What's deliberately missing

- No structs or classes.
- No modules, exceptions, or string formatting beyond concat.
- No dicts. Lists are heterogeneous, indexable, and appendable via
  `append(xs, val)`, but only in the interpreter — the transpiler doesn't
  handle heterogeneous lists in `--exec` mode.
- Slicing is copy, not view.  Matrices can't be sliced yet (only vecs and
  strings).
- Tag-checking is local: a tag stays attached to a value but isn't a real
  type and doesn't constrain function signatures.  A library function that
  takes a "vec of length rows(A)" can't express that in the signature; use
  `at`/`set` to opt out of checks at the function boundary.
- Performance: tree-walking, ~50-100× slower than C for scalar code.
  Builtins are the fast path; for hot loops you'd call into linalg ops.
- `--exec` is a strict subset of the interpreter.  The transpiler doesn't
  yet handle heterogeneous lists, closures, default args, matrix literals,
  slicing, string concat, or function-arg-typed parameters beyond the
  numerical `double (*)(double)` shape.  Tagged-index checks are erased
  at transpile time.  Programs using interpreter-only features run fine
  with `./knot FILE` but error out under `./knot --exec FILE`.  The
  authoritative list is the comment at the top of `src/codegen.hpp`.

## Bugs surfaced during construction

These are kept in the README because they were instructive:

1. `Function::body` and `param_defaults` are raw pointers into the AST.
   For file mode the AST lives until `main` returns, so this is fine.
   For the REPL, each parsed line went out of scope after running, leaving
   function bodies dangling.  Fixed by `retained.push_back(std::move(program))`
   in the REPL loop.
2. The first version treated `{ }` as an "expression grouping" that
   suppressed newlines inside.  Wrong: `{ }` is a *statement block*; the
   newlines inside it are the statement terminators we depend on.  Only
   `( )` and `[ ]` suppress newlines.

## License

MIT. See [LICENSE](LICENSE).
