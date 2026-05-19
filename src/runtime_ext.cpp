// knot runtime extension.
//
// This is a small C++ shim that exposes selected C++ stdlib functionality
// via a flat C ABI.  The transpiler emits plain C that calls into these
// functions.  knot users never see C++ types or template errors -- the
// boundary is C all the way through.
//
// What lives here:
//   * sort_vec(v)            -- in-place std::sort
//   * rng_seed(seed)         -- seed the global mt19937_64
//   * rng_uniform()          -- uniform [0, 1)
//   * rng_normal()           -- standard normal (mean 0, var 1)
//   * read_csv(path, &rows, &cols) -> double*  -- load a numeric CSV
//
// All allocations returned across the boundary are plain malloc'd doubles
// so knot's existing free strategy (drop on exit) keeps working.

#include "runtime.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

extern "C" {

// ---- Sort -----------------------------------------------------------------

// In-place ascending sort on a vec's data.  std::sort is O(n log n).
void knot_sort_vec(knot_vec v) {
    std::sort(v.data, v.data + v.n);
}


// ---- Random ---------------------------------------------------------------

// Global RNG state.  Single-threaded; one process worth.
namespace {
    std::mt19937_64& rng() {
        static std::mt19937_64 r(1234567ULL);
        return r;
    }
    std::uniform_real_distribution<double>& unif() {
        static std::uniform_real_distribution<double> d(0.0, 1.0);
        return d;
    }
    std::normal_distribution<double>& nrm() {
        static std::normal_distribution<double> d(0.0, 1.0);
        return d;
    }
}

void knot_rng_seed(double seed) {
    // Convert to uint64_t in a way that handles negative and non-integer
    // seeds reasonably.
    uint64_t s;
    if (seed >= 0 && seed < 1.8e19) {
        s = (uint64_t)seed;
    } else {
        // Hash the bit pattern for negative / huge seeds.
        uint64_t bits;
        std::memcpy(&bits, &seed, sizeof(bits));
        s = bits;
    }
    rng().seed(s);
}

double knot_rng_uniform(void) {
    return unif()(rng());
}

double knot_rng_normal(void) {
    return nrm()(rng());
}


// ---- CSV reader -----------------------------------------------------------

// Read a numeric CSV file.  Returns a knot_mat (column-major), or a zero-
// shape mat on failure.  Treats the first row as data, not headers -- the
// caller can strip them with slicing if needed (when we have it).
//
// Whitespace and commas are field separators.  Empty fields read as 0.0.
// Lines starting with '#' are treated as comments.  Any field that doesn't
// parse cleanly as a number causes an error message and a zero return.
knot_mat knot_read_csv(const char* path) {
    knot_mat empty;
    empty.data = nullptr;
    empty.rows = 0;
    empty.cols = 0;

    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "read_csv: cannot open '%s'\n", path);
        return empty;
    }

    std::vector<std::vector<double>> rows;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        // Strip an optional trailing '\r' from CRLF files.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip blanks and comments.
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line[first] == '#') continue;

        std::vector<double> cells;
        // Replace separators with spaces so we can use a single stream.
        for (char& c : line) {
            if (c == ',' || c == ';' || c == '\t') c = ' ';
        }
        std::istringstream ss(line);
        double x;
        while (ss >> x) {
            cells.push_back(x);
        }
        if (!ss.eof()) {
            std::fprintf(stderr,
                "read_csv: parse error at %s:%d\n", path, lineno);
            return empty;
        }
        if (!cells.empty()) rows.push_back(std::move(cells));
    }

    if (rows.empty()) return empty;

    // Check all rows have the same width.
    size_t cols = rows[0].size();
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() != cols) {
            std::fprintf(stderr,
                "read_csv: %s has inconsistent row widths "
                "(row 1 has %zu, row %zu has %zu)\n",
                path, cols, i + 1, rows[i].size());
            return empty;
        }
    }

    // Allocate and fill the mat in column-major order to match knot's
    // storage convention.
    knot_mat m = knot_mat_new((int)rows.size(), (int)cols);
    for (size_t j = 0; j < cols; ++j) {
        for (size_t i = 0; i < rows.size(); ++i) {
            m.data[i + j * m.rows] = rows[i][j];
        }
    }
    return m;
}

// ---- Write CSV ------------------------------------------------------------

// Write a numeric mat as CSV.  Returns 1 on success, 0 on failure.
int knot_write_csv(knot_mat m, const char* path) {
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "write_csv: cannot open '%s'\n", path);
        return 0;
    }
    for (int i = 0; i < m.rows; ++i) {
        for (int j = 0; j < m.cols; ++j) {
            if (j) f << ',';
            f << m.data[i + (size_t)j * m.rows];
        }
        f << '\n';
    }
    return 1;
}

} // extern "C"
