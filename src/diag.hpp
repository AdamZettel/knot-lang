#pragma once
#include "span.hpp"
#include <stdexcept>
#include <string>

namespace knot {

// A diagnostic carries a span so we can render a caret under the offending text.
// We use this for both parse errors and runtime errors.
struct Diag : public std::runtime_error {
    Span span;
    Diag(Span s, const std::string& msg) : std::runtime_error(msg), span(s) {}
};

} // namespace knot
