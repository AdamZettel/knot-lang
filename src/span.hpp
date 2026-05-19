#pragma once
#include <cstddef>
#include <string>

namespace knot {

// A source location: byte offset, line, column (all 1-indexed for line/col).
struct Span {
    size_t start = 0;   // byte offset into source
    size_t length = 0;  // length in bytes
    int line = 1;       // 1-indexed line of `start`
    int col = 1;        // 1-indexed column of `start`

    static Span merge(const Span& a, const Span& b) {
        Span s;
        s.start = a.start;
        s.length = (b.start + b.length) - a.start;
        s.line = a.line;
        s.col = a.col;
        return s;
    }
};

} // namespace knot
