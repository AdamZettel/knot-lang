// knot runtime, C version.
// The transpiler emits calls into the functions declared here. Everything
// is plain C, malloc-on-the-heap, no refcounting in v1 — numerical scripts
// run and terminate, the OS reclaims memory. Fine for v1.

#ifndef KNOT_RUNTIME_H
#define KNOT_RUNTIME_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Vector --------------------------------------------------------------

typedef struct {
    double* data;
    int n;
} knot_vec;

static inline knot_vec knot_vec_new(int n) {
    knot_vec v;
    v.n = n;
    v.data = (double*)calloc((size_t)n, sizeof(double));
    return v;
}

static inline knot_vec knot_vec_from(int n, const double* src) {
    knot_vec v = knot_vec_new(n);
    memcpy(v.data, src, (size_t)n * sizeof(double));
    return v;
}

static inline double knot_vec_get(knot_vec v, int i) {
    if (i < 0) i += v.n;
    if (i < 0 || i >= v.n) {
        fprintf(stderr, "vec index %d out of range [0, %d)\n", i, v.n);
        exit(1);
    }
    return v.data[i];
}

static inline void knot_vec_set(knot_vec v, int i, double x) {
    if (i < 0) i += v.n;
    if (i < 0 || i >= v.n) {
        fprintf(stderr, "vec index %d out of range [0, %d)\n", i, v.n);
        exit(1);
    }
    v.data[i] = x;
}

static inline knot_vec knot_vec_add(knot_vec a, knot_vec b) {
    if (a.n != b.n) { fprintf(stderr, "vec size mismatch\n"); exit(1); }
    knot_vec r = knot_vec_new(a.n);
    for (int i = 0; i < a.n; ++i) r.data[i] = a.data[i] + b.data[i];
    return r;
}

static inline knot_vec knot_vec_sub(knot_vec a, knot_vec b) {
    if (a.n != b.n) { fprintf(stderr, "vec size mismatch\n"); exit(1); }
    knot_vec r = knot_vec_new(a.n);
    for (int i = 0; i < a.n; ++i) r.data[i] = a.data[i] - b.data[i];
    return r;
}

static inline knot_vec knot_vec_scale(knot_vec a, double s) {
    knot_vec r = knot_vec_new(a.n);
    for (int i = 0; i < a.n; ++i) r.data[i] = a.data[i] * s;
    return r;
}

static inline double knot_vec_dot(knot_vec a, knot_vec b) {
    if (a.n != b.n) { fprintf(stderr, "vec size mismatch\n"); exit(1); }
    double s = 0;
    for (int i = 0; i < a.n; ++i) s += a.data[i] * b.data[i];
    return s;
}

static inline double knot_vec_norm(knot_vec a) {
    return sqrt(knot_vec_dot(a, a));
}

static inline int knot_vec_len(knot_vec a) { return a.n; }

// ---- Matrix (column-major) ----------------------------------------------

typedef struct {
    double* data;
    int rows;
    int cols;
} knot_mat;

static inline knot_mat knot_mat_new(int r, int c) {
    knot_mat m;
    m.rows = r; m.cols = c;
    m.data = (double*)calloc((size_t)(r * c), sizeof(double));
    return m;
}

static inline double knot_mat_get(knot_mat m, int i, int j) {
    if (i < 0) i += m.rows;
    if (j < 0) j += m.cols;
    if (i < 0 || i >= m.rows || j < 0 || j >= m.cols) {
        fprintf(stderr, "mat index (%d, %d) out of range (%d, %d)\n",
                i, j, m.rows, m.cols);
        exit(1);
    }
    return m.data[i + (size_t)j * m.rows];
}

static inline void knot_mat_set(knot_mat m, int i, int j, double x) {
    if (i < 0) i += m.rows;
    if (j < 0) j += m.cols;
    if (i < 0 || i >= m.rows || j < 0 || j >= m.cols) {
        fprintf(stderr, "mat index (%d, %d) out of range (%d, %d)\n",
                i, j, m.rows, m.cols);
        exit(1);
    }
    m.data[i + (size_t)j * m.rows] = x;
}

static inline int knot_mat_rows(knot_mat m) { return m.rows; }
static inline int knot_mat_cols(knot_mat m) { return m.cols; }

static inline knot_mat knot_mat_mul(knot_mat a, knot_mat b) {
    if (a.cols != b.rows) { fprintf(stderr, "mat shape mismatch\n"); exit(1); }
    knot_mat r = knot_mat_new(a.rows, b.cols);
    for (int j = 0; j < b.cols; ++j) {
        for (int k = 0; k < a.cols; ++k) {
            double bkj = b.data[k + (size_t)j * b.rows];
            for (int i = 0; i < a.rows; ++i) {
                r.data[i + (size_t)j * a.rows] += a.data[i + (size_t)k * a.rows] * bkj;
            }
        }
    }
    return r;
}

static inline knot_mat knot_mat_add(knot_mat a, knot_mat b) {
    if (a.rows != b.rows || a.cols != b.cols) {
        fprintf(stderr, "mat add: shape mismatch\n"); exit(1);
    }
    knot_mat r = knot_mat_new(a.rows, a.cols);
    int n = a.rows * a.cols;
    for (int i = 0; i < n; ++i) r.data[i] = a.data[i] + b.data[i];
    return r;
}

static inline knot_mat knot_mat_sub(knot_mat a, knot_mat b) {
    if (a.rows != b.rows || a.cols != b.cols) {
        fprintf(stderr, "mat sub: shape mismatch\n"); exit(1);
    }
    knot_mat r = knot_mat_new(a.rows, a.cols);
    int n = a.rows * a.cols;
    for (int i = 0; i < n; ++i) r.data[i] = a.data[i] - b.data[i];
    return r;
}

static inline knot_mat knot_mat_transpose(knot_mat a) {
    knot_mat r = knot_mat_new(a.cols, a.rows);
    for (int j = 0; j < a.cols; ++j) {
        for (int i = 0; i < a.rows; ++i) {
            r.data[j + (size_t)i * a.cols] = a.data[i + (size_t)j * a.rows];
        }
    }
    return r;
}

static inline knot_vec knot_mat_vec(knot_mat a, knot_vec v) {
    if (a.cols != v.n) { fprintf(stderr, "matvec shape mismatch\n"); exit(1); }
    knot_vec r = knot_vec_new(a.rows);
    for (int j = 0; j < a.cols; ++j) {
        for (int i = 0; i < a.rows; ++i) {
            r.data[i] += a.data[i + (size_t)j * a.rows] * v.data[j];
        }
    }
    return r;
}

// ---- Print ---------------------------------------------------------------

// Print a double in the same format as the interpreter: integral values
// without trailing ".0".
static inline void knot_print_num(double x) {
    if (x == (long long)x && x > -1e16 && x < 1e16) {
        printf("%lld", (long long)x);
    } else {
        printf("%g", x);
    }
}

static inline void knot_print_vec(knot_vec v) {
    putchar('[');
    for (int i = 0; i < v.n; ++i) {
        if (i) printf(", ");
        knot_print_num(v.data[i]);
    }
    putchar(']');
}

static inline void knot_print_mat(knot_mat m) {
    putchar('[');
    for (int i = 0; i < m.rows; ++i) {
        if (i) printf(",\n ");
        putchar('[');
        for (int j = 0; j < m.cols; ++j) {
            if (j) printf(", ");
            knot_print_num(m.data[i + (size_t)j * m.rows]);
        }
        putchar(']');
    }
    putchar(']');
}

static inline void knot_print_str(const char* s) {
    fputs(s, stdout);
}

static inline void knot_print_newline(void) { putchar('\n'); }

// Runtime toggle for `narrate` statements. The transpiler emits
// `if (knot_narration_on) { ... }` around each narrate's prints so
// --no-narrate at runtime suppresses them without recompiling. The
// default is 1 (on); a --no-narrate option in a future launcher
// would set this to 0 at startup.
static int knot_narration_on = 1;

// ---- N-D tensor (row-major) ---------------------------------------------
// Storage for arbitrary-rank tensors. Two-electron integrals (mu nu | lam
// sig) and any higher-rank intermediates (post-HF amplitudes, etc.) live
// here. Indexing is row-major: idx = i_0 * d_1 * d_2 * ... + i_1 * d_2 *
// ... + ... + i_{rank-1}. The dims array lives alongside the data.

typedef struct {
    double* data;
    int* dims;       // length `rank`
    int rank;
    int size;        // product of dims (cached)
} knot_tensor;

static inline knot_tensor knot_tensor_new(int rank, const int* dims) {
    knot_tensor t;
    t.rank = rank;
    t.dims = (int*)malloc((size_t)rank * sizeof(int));
    int sz = 1;
    for (int i = 0; i < rank; ++i) { t.dims[i] = dims[i]; sz *= dims[i]; }
    t.size = sz;
    t.data = (double*)calloc((size_t)sz, sizeof(double));
    return t;
}

static inline int knot_tensor_flat(knot_tensor t, const int* idx) {
    int f = 0;
    for (int i = 0; i < t.rank; ++i) {
        int ix = idx[i];
        if (ix < 0) ix += t.dims[i];
        if (ix < 0 || ix >= t.dims[i]) {
            fprintf(stderr, "tensor index %d out of range on axis %d (dim %d)\n",
                    idx[i], i, t.dims[i]);
            exit(1);
        }
        f = f * t.dims[i] + ix;
    }
    return f;
}

static inline double knot_tensor_get(knot_tensor t, const int* idx) {
    return t.data[knot_tensor_flat(t, idx)];
}

static inline void knot_tensor_set(knot_tensor t, const int* idx, double v) {
    t.data[knot_tensor_flat(t, idx)] = v;
}

static inline int knot_tensor_rank(knot_tensor t) { return t.rank; }
static inline int knot_tensor_dim(knot_tensor t, int axis) {
    if (axis < 0 || axis >= t.rank) {
        fprintf(stderr, "tensor dim: axis %d out of range\n", axis);
        exit(1);
    }
    return t.dims[axis];
}

// ---- Closures -----------------------------------------------------------
// Numerical callbacks (the arg to simpson, bisect, rk4, etc.) are passed
// as fat pointers: a function pointer plus a void* env. Bare top-level
// `def`s get an env-ignoring thunk and a NULL env; `fn(x) -> EXPR`
// captures its free variables into a heap-allocated env struct.
//
// One closure type per arity. The env-side of the function pointer is
// always void*; the lifted function casts it to the concrete env-struct
// type it knows about.

typedef struct {
    double (*fn)(void* env, double);
    void*  env;
} knot_clos_d_d;

typedef struct {
    double (*fn)(void* env, double, double);
    void*  env;
} knot_clos_dd_d;

typedef struct {
    double (*fn)(void* env, double, double, double);
    void*  env;
} knot_clos_ddd_d;

// ---- Math forwards ------------------------------------------------------

static inline double knot_abs(double x)  { return fabs(x); }
static inline double knot_sqrt(double x) { return sqrt(x); }
static inline double knot_sin(double x)  { return sin(x); }
static inline double knot_cos(double x)  { return cos(x); }
static inline double knot_exp(double x)  { return exp(x); }
static inline double knot_log(double x)  { return log(x); }

// ---- Extension functions (linked from runtime_ext.cpp) ------------------
// These wrap selected C++ stdlib facilities behind a flat C ABI.  knot
// programs that use sort/randn/read_csv pull these in at link time; programs
// that don't can skip linking the .o entirely (the transpiler's --exec path
// always links it for simplicity).
#ifdef __cplusplus
extern "C" {
#endif

void   knot_sort_vec(knot_vec v);

void   knot_rng_seed(double seed);
double knot_rng_uniform(void);
double knot_rng_normal(void);

knot_mat knot_read_csv(const char* path);
int      knot_write_csv(knot_mat m, const char* path);

#ifdef __cplusplus
}
#endif

#endif // KNOT_RUNTIME_H
