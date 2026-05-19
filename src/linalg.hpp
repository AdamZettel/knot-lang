#pragma once
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace knot {

// A "shape tag" is the provenance of a number that originated from a shape
// query (rows(A), cols(A), len(v)) or from iterating with `loop`. When a
// number is later used to index a vec/mat, and BOTH the index and the
// container's axis carry tags, the interpreter verifies they match -- catching
// swap/wrong-matrix bugs that bounds-checking alone misses.
//
// container_id is the address of the originating storage, stringified.
// axis: 0 = matrix rows, 1 = matrix cols, -1 = vec length.
struct ShapeTag {
    std::string container_id;
    int axis = 0;
    std::string display;
};

// 1-D vector of doubles. Optionally carries a length tag (set when the vec
// was constructed with a tagged length, e.g. zeros(rows(A))).
struct Vec {
    std::vector<double> data;
    std::shared_ptr<ShapeTag> len_tag;

    Vec() = default;
    explicit Vec(size_t n, double fill = 0.0) : data(n, fill) {}
    explicit Vec(std::vector<double> d) : data(std::move(d)) {}

    size_t size() const { return data.size(); }
    double& operator[](size_t i) { return data[i]; }
    double operator[](size_t i) const { return data[i]; }
};

// Column-major dense matrix of doubles.  Optionally carries row and column
// shape tags (set when constructed with tagged dimensions).
struct Mat {
    size_t rows = 0;
    size_t cols = 0;
    std::vector<double> data;
    std::shared_ptr<ShapeTag> row_tag;
    std::shared_ptr<ShapeTag> col_tag;

    Mat() = default;
    Mat(size_t r, size_t c, double fill = 0.0)
        : rows(r), cols(c), data(r * c, fill) {}

    double& at(size_t i, size_t j) { return data[i + j * rows]; }
    double at(size_t i, size_t j) const { return data[i + j * rows]; }
};

// ---- Vec ops -----------------------------------------------------------

inline Vec vec_add(const Vec& a, const Vec& b) {
    if (a.size() != b.size())
        throw std::runtime_error("vector size mismatch in '+': "
            + std::to_string(a.size()) + " vs " + std::to_string(b.size()));
    Vec r(a.size());
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i] + b[i];
    return r;
}

inline Vec vec_sub(const Vec& a, const Vec& b) {
    if (a.size() != b.size())
        throw std::runtime_error("vector size mismatch in '-': "
            + std::to_string(a.size()) + " vs " + std::to_string(b.size()));
    Vec r(a.size());
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i] - b[i];
    return r;
}

inline Vec vec_scale(const Vec& a, double s) {
    Vec r(a.size());
    for (size_t i = 0; i < a.size(); ++i) r[i] = a[i] * s;
    return r;
}

inline double vec_dot(const Vec& a, const Vec& b) {
    if (a.size() != b.size())
        throw std::runtime_error("vector size mismatch in 'dot': "
            + std::to_string(a.size()) + " vs " + std::to_string(b.size()));
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
    return s;
}

inline double vec_norm(const Vec& a) {
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) s += a[i] * a[i];
    return std::sqrt(s);
}

// ---- Mat ops -----------------------------------------------------------

inline Mat mat_add(const Mat& a, const Mat& b) {
    if (a.rows != b.rows || a.cols != b.cols)
        throw std::runtime_error("matrix shape mismatch in '+': "
            + std::to_string(a.rows) + "x" + std::to_string(a.cols) + " vs "
            + std::to_string(b.rows) + "x" + std::to_string(b.cols));
    Mat r(a.rows, a.cols);
    for (size_t k = 0; k < a.data.size(); ++k) r.data[k] = a.data[k] + b.data[k];
    return r;
}

inline Mat mat_sub(const Mat& a, const Mat& b) {
    if (a.rows != b.rows || a.cols != b.cols)
        throw std::runtime_error("matrix shape mismatch in '-': "
            + std::to_string(a.rows) + "x" + std::to_string(a.cols) + " vs "
            + std::to_string(b.rows) + "x" + std::to_string(b.cols));
    Mat r(a.rows, a.cols);
    for (size_t k = 0; k < a.data.size(); ++k) r.data[k] = a.data[k] - b.data[k];
    return r;
}

inline Mat mat_scale(const Mat& a, double s) {
    Mat r(a.rows, a.cols);
    for (size_t k = 0; k < a.data.size(); ++k) r.data[k] = a.data[k] * s;
    return r;
}

// Standard matrix-matrix multiply: (m x k) * (k x n) -> (m x n).
// Column-major triple loop; inner loop walks columns of the result.
inline Mat mat_mul(const Mat& a, const Mat& b) {
    if (a.cols != b.rows)
        throw std::runtime_error("matrix shape mismatch in 'matmul': "
            + std::to_string(a.rows) + "x" + std::to_string(a.cols) + " * "
            + std::to_string(b.rows) + "x" + std::to_string(b.cols));
    Mat r(a.rows, b.cols);
    for (size_t j = 0; j < b.cols; ++j) {
        for (size_t k = 0; k < a.cols; ++k) {
            double bkj = b.at(k, j);
            for (size_t i = 0; i < a.rows; ++i) {
                r.at(i, j) += a.at(i, k) * bkj;
            }
        }
    }
    return r;
}

// Matrix-vector multiply: (m x n) * (n) -> (m)
inline Vec mat_vec(const Mat& a, const Vec& v) {
    if (a.cols != v.size())
        throw std::runtime_error("shape mismatch in 'matvec': "
            + std::to_string(a.rows) + "x" + std::to_string(a.cols) + " * "
            + std::to_string(v.size()));
    Vec r(a.rows);
    for (size_t j = 0; j < a.cols; ++j) {
        double vj = v[j];
        for (size_t i = 0; i < a.rows; ++i) {
            r[i] += a.at(i, j) * vj;
        }
    }
    return r;
}

inline Mat mat_transpose(const Mat& a) {
    Mat r(a.cols, a.rows);
    for (size_t j = 0; j < a.cols; ++j)
        for (size_t i = 0; i < a.rows; ++i)
            r.at(j, i) = a.at(i, j);
    return r;
}

inline Mat mat_eye(size_t n) {
    Mat r(n, n);
    for (size_t i = 0; i < n; ++i) r.at(i, i) = 1.0;
    return r;
}

} // namespace knot
