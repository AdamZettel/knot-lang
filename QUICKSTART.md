# Quickstart

This is the fastest path to having knot working on your machine and
seeing what it can do. Five minutes.

## 1. Build

You need `g++` (or `clang++`) for C++17, and `cc` (or `gcc`/`clang`) for
the transpiler's compile step. Both are present by default on Linux and on
macOS with Xcode command-line tools.

```bash
cd knot
make
```

That's it. One `g++` invocation, no dependencies, takes a few seconds.
Produces a binary called `knot` in the current directory.

## 2. Verify

```bash
./knot examples/tour.knot
```

Should print a long list of numbers, vectors, matrices ending in `1 2 3`.
If that works, the interpreter is healthy.

```bash
./knot --exec examples/bisect_annotated.knot
```

Should print `1.41421`. That confirms the transpiler is finding `cc` and
the runtime header. If it errors out with "could not locate runtime.h",
make sure you're running from inside the `knot/` directory.

## 3. Real demo: option pricer

```bash
./knot examples/option_pricer.knot
```

Prices a European call/put under Black-Scholes three ways: closed-form,
Monte Carlo, and a binomial tree.  Runs interpreted in about a second.
Black-Scholes gives 10.4506; the binomial tree converges to it; Monte
Carlo is noisy at 1000 paths but in the right ballpark.

Now run it compiled:

```bash
./knot --exec examples/option_pricer.knot
```

First run takes ~150ms (most of it the C compiler). Re-run and it's ~4ms
because the binary is cached. For real speed, edit the file and crank
`n_mc = 1000` up to `n_mc = 1000000` — at a million MC paths it still
finishes faster than the interpreter does with a thousand.

## 4. Iterate

The intended workflow is the Python-script loop: edit the file in your
editor, save, run from terminal. There's no notebook, no kernel to keep
warm, no environment. The interpreter starts in 3ms.

If you want to keep something interactive, the REPL is there:

```bash
./knot
>>> v = [3, 1, 4, 1, 5]
>>> sort(v)
[1, 1, 3, 4, 5]
>>> lslib
  arange    Vector [a, a+step, ...] up to but not including b.
  argmax    Index of the largest element of v.
  ...
>>> whatis solve
solve(A, b, x_out)
  Solve A x = b for x, via LU with partial pivoting.
  x_out should be a vec of length rows(A); it will be filled with the solution.
```

## 5. When you hit something weird

`./knot --cc your_file.knot` prints the C the transpiler would have
compiled. Reading that often makes a confusing error message obvious.

For interpreter-only debugging, the error messages should be reasonably
specific. If they're not, that's a real complaint — file it (mentally;
there's no issue tracker yet) and the next session can sharpen them.

## Stuff that doesn't work yet

- No CSV/file I/O. For data, hardcode it in the source for now.
- No plotting. Save numbers to a file via shell redirect, then plot
  outside knot.
- No FFI to C libraries. Coming.

## What the directory contains

```
knot/
├── Makefile               # one g++ invocation, no fancy stuff
├── README.md              # the longer reference
├── QUICKSTART.md          # this file
├── src/                   # ~3000 lines of C++17
│   ├── runtime.h          # C runtime that --exec output links against
│   ├── codegen.hpp        # AST → C
│   ├── interpreter.hpp    # AST → values
│   └── ...                # lexer, parser, value, etc
├── stdlib/
│   └── stdlib.knot          # numerical library, embedded at build time
├── examples/
│   ├── option_pricer.knot   # Black-Scholes + MC + binomial
│   ├── tour.knot            # basics walkthrough
│   ├── for_tour.knot        # for-loop + tags + stdlib
│   └── bisect_annotated.knot + .annot   # annotation system demo
└── scripts/
    └── embed_stdlib.sh    # turns stdlib.knot into a C++ string constant
```
