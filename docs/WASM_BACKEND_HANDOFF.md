# WASM backend handoff

A pointer to the knot repo for the C+WASM-backend-library effort.

**Repo:** https://github.com/AdamZettel/knot-lang
**Branch:** main
**Relevant commits at the time of this doc:**
- `4d35157` — WASM backend foundation: binary writer + stub codegen + `--wasm-test`
- `087ed99` — WASM backend phase 2: numerical core (f64 arithmetic, if/else, while, locals, print)

## What knot is

A small numerical scripting language with a tree-walking interpreter
(`--interp`, the default) and a transpile-to-C `--exec` mode that
shells out to the system C compiler. ~5800 LOC across `src/`. Has
shape-tagged indices, omniscient debugging, an N-D tensor type, a
Jacobi eigensolver in the stdlib, an Obara-Saika electronic-structure
example program, and a browser playground compiled to WASM.

This handoff is about a *new third backend*: `--wasm`, which emits a
WebAssembly module directly from the knot AST so the browser
playground can run user code at near-native speed without a C
compiler in the runtime.

## Where the WASM backend lives

Two new headers and one CLI flag. Read in this order.

### `src/wasm_writer.hpp` (~250 LOC)

Low-level binary serialization. No external dependencies.

- `WasmWriter`: byte buffer + `u8`, `u32_le`, unsigned/signed LEB128,
  IEEE-754 `f64` little-endian, length-prefixed UTF-8 strings.
- `WasmModule`: tracks types, imports, function declarations, memory,
  exports. `emit()` serializes the canonical section order
  (1, 2, 3, 5, 7, 10) with sizes computed at serialization time.
- Opcode and type constants in nested namespaces
  (`wasm::op::F64_ADD`, `wasm::type::F64`).

Hand-rolled, not via binaryen / wabt. The WASM binary format is small
and stable; the dependency would save nothing.

### `src/wasm_codegen.hpp` (~280 LOC)

Walks knot's AST and emits into a `WasmModule`.

- `WasmCodegen::compile(program, filename) -> std::vector<uint8_t>`.
- `Ctx` struct: `unordered_map<string, uint32_t> locals` (lazy
  index assignment on first reference) + cached import indices.
- All values flow as `f64` in this phase. Bool is `0.0` / `1.0` — each
  comparison opcode is followed by `F64_CONVERT_I32_S` so the rest of
  the codegen treats bool-as-f64 uniformly.
- One imported function so far: `env.knot_print_num(f64) -> ()`.

### `src/main.cpp`

Search for `Mode::Wasm`. Two flags wired:

- `--wasm FILE.knot` → writes `FILE.wasm` next to the source.
- `--wasm-test FILE.wasm` → writes a fixed "main returns 42" module
  for binary-writer sanity-checking.

## What the WASM backend handles today

- `NumberLit`, `BoolLit`, `Ident` (locals only).
- Unary `-`, `!`.
- Binary `+ - * / == != < <= > >=`.
- `Assign` (plain name only), `CompAssign` (plain name only).
- `If` with optional `else`.
- `While` lowered to `block { loop { brif !cond, out; body; br loop } }`.
- `ExprStmt` (with `DROP`).
- `print(num)` lowered to `CALL` to the imported `knot_print_num`.

## What's not yet there

Roughly in order of complexity:

1. **`for X to N` loops.** Needs i32 loop counters; current locals
   table writes f64 only.
2. **User-defined `def`s.** Each becomes a WASM function; calls are
   `CALL` (direct) or `CALL_INDIRECT` through a function table for
   first-class function values.
3. **Math builtins** (`sin`, `cos`, `exp`, `log`, `sqrt`, `pow`).
   JS-imported, trivial to wire.
4. **`vec` / `mat` / `tensor`.** Knot's three numerical containers.
   Needs a linear-memory allocator (bump arena fits — knot leaks),
   header layouts matching the C runtime structs, indexed read/write.
5. **Runtime helpers** (`knot_vec_new`, `knot_mat_mul`,
   `knot_tensor_get`, ...). Defined in C in `src/runtime.h`. Plan was
   to compile this file once via `emcc -sSIDE_MODULE=1` and import the
   symbols. Requires `emcc` on the build machine; not yet installed.
6. **Closure ABI in WASM.** Knot's existing fat-pointer scheme
   (`{fn_ptr, env_ptr}`) lifted to WASM function tables + env structs
   in linear memory. The C codegen already does closure conversion;
   the analysis is reusable (`src/parser.hpp::collect_free_vars`).
7. **Playground integration.** Wire the WASM backend through to the
   browser playground's `Run` button.

## Cross-mode parity smoke tests

`scripts/smoke_test.sh` "wasm backend foundation" section. Each test
compiles a knot program through `--wasm`, instantiates the resulting
module under Node with a JS-side `knot_print_num` callback, and
compares stdout to a `--interp` run. Currently four programs cover
constants + arithmetic, locals + while, if/else, and compound assign.
63 smoke total passing.

## Key design choices made

These are the bits a library could replace cleanly without disrupting
the rest of the codebase:

- **Hand-rolled binary writer.** ~250 LOC, no build deps. If your
  library offers this, substitution is trivial: `WasmWriter`'s API is
  `u8 / uleb / sleb / f64_le / name / raw` and `WasmModule` just
  tracks vectors.
- **All values as f64 in the scalar phase.** Works for the numerical
  core; doesn't survive aggregates. The C codegen has a richer
  `ExprResult { code, CType }` shape — see `src/codegen.hpp`.
- **Imported I/O only.** JS host owns stdout; the WASM module is pure
  compute. Same shape as WASI but with a much smaller import surface
  (currently one function).
- **Structured control flow.** `if/while` lower to WASM
  `IF/BLOCK/LOOP/BR_IF` rather than label-and-goto. Reuses what WASM
  gives us.

## What a general C+WASM backend library should provide

In rough priority order, based on what knot's codegen wanted:

1. **`BinaryWriter` + `Module` builder.** Direct replacement for
   `wasm_writer.hpp`. Bonus support for custom sections, the name
   section (debugging), data section, table section.
2. **`FunctionBuilder` with locals + structured-control-flow helpers.**
   Currently I push raw opcode bytes into a `vector<uint8_t>` and call
   `emit_uleb` for indices. A real library would give me
   `fn.local("x", f64)`, `fn.if_(cond)`, `fn.while_(cond)`,
   `fn.call(import_idx)` and handle the bookkeeping.
3. **Linear-memory allocator abstraction.** Bump arena vs
   reference-counted vs `malloc`-equivalent. Knot picks "leak" as the
   default; a general library should offer the menu.
4. **C-to-WASM runtime linking.** The killer feature. Language
   implementers want to write runtime code in C (math.h, easy to
   debug), then ship the WASM result as a linkable module the codegen
   imports against. At minimum: take a `.c` file, build it with
   `emcc -sSIDE_MODULE=1` at library-build time, expose a "link this
   in" entry point. Bonus: a CLI to inspect the exported symbols of
   an existing `.wasm` so the codegen knows what's available.
5. **Closure-conversion helper.** Free-var analysis on an AST
   (see knot's `src/parser.hpp::collect_free_vars`), env-struct
   layout, function-table indirection. The C codegen has a working
   FnExpr lifting pass (`src/codegen.hpp::emit_fnexpr`); the WASM
   version would benefit from a shared utility.
6. **JS-side bridge generator.** For each imported function the
   codegen needs (print, plot, file I/O, console), emit the matching
   JS adapter. Currently the smoke test hand-writes the JS; a library
   should generate this.

## What the library should not impose on knot

- An AST visitor pattern. Knot has its own; libraries shouldn't
  dictate one.
- A type system. Knot has shape-tagged indices, closures, an N-D
  tensor, all very knot-flavored.
- The interpreter / dual-mode parity invariant. Knot's identity.

## Additional reading

For understanding what knot's WASM backend will eventually need:

- `src/runtime.h` — the C runtime knot's existing `--exec` links
  against. This is what needs porting (or linking) for the WASM path.
- `src/codegen.hpp` — the C codegen. Most of the structure (Ctx,
  ExprResult, type inference, FnExpr lifting, closure ABI) translates
  directly to a WASM target.
- `examples/electronic_structure/01_h2o_sto3g.knot` — the most
  ambitious knot program. Computes the Hartree-Fock energy of H2O
  to textbook precision in pure knot. The eventual goal is running
  this in the browser at native `--exec` speed.

## Contact / questions

The handoff doc this README summarizes is in the same directory:
`docs/WASM_BACKEND_HANDOFF.md` (this file). For the longer-form
write-up of the design decisions, see the message thread that
produced commits `4d35157` and `087ed99`.
