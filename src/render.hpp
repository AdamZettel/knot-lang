#pragma once
#include "diag.hpp"
#include <iostream>
#include <string>

namespace knot {

// Render a diagnostic with the offending source line and a caret. Output is
// stylistically similar to rustc/clang:
//
//   error: undefined variable 'foo'
//    --> file:3:5
//     |
//   3 |     foo + 1;
//     |     ^^^
//
inline void render_diag(const std::string& filename,
                        const std::string& src,
                        const Diag& d) {
    std::cerr << "error: " << d.what() << "\n";
    std::cerr << " --> " << filename << ":" << d.span.line << ":" << d.span.col << "\n";

    // If the span points outside the user source (e.g. it came from stdlib
    // during a transpile), we can't render a source line. Show a hint.
    if (d.span.start >= src.size()) {
        std::cerr << "    (location in embedded stdlib; "
                  << "source not displayed)\n";
        return;
    }

    // Find the start of the offending line.
    size_t line_start = d.span.start;
    while (line_start > 0 && src[line_start - 1] != '\n') --line_start;
    // Find the end of the line.
    size_t line_end = d.span.start;
    while (line_end < src.size() && src[line_end] != '\n') ++line_end;

    std::string line_text = src.substr(line_start, line_end - line_start);
    std::string line_num = std::to_string(d.span.line);
    std::string pad(line_num.size(), ' ');

    std::cerr << pad      << " |\n";
    std::cerr << line_num << " | " << line_text << "\n";
    std::cerr << pad      << " | ";

    // Print spaces up to the caret column.
    for (int i = 1; i < d.span.col; ++i) std::cerr << ' ';
    size_t caret_len = d.span.length > 0 ? d.span.length : 1;
    if (d.span.col > 0 && (size_t)d.span.col - 1 <= line_text.size()) {
        size_t remaining = line_text.size() - (d.span.col - 1);
        if (caret_len > remaining) caret_len = remaining > 0 ? remaining : 1;
    } else {
        caret_len = 1;
    }
    for (size_t i = 0; i < caret_len; ++i) std::cerr << '^';
    std::cerr << "\n";
}

} // namespace knot
