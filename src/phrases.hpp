#pragma once
// Plain-speak phrase table for the `take ...` syntax. Each pattern is
// a sequence of literal words plus hole markers ("_"); matching is
// first-match-wins (longest patterns must come first), and each hole
// is re-parsed as a knot expression in parser.hpp.
//
// New patterns added here should ALWAYS point at an existing builtin
// or stdlib function -- the phrase syntax is sugar, never a place to
// invent new semantics. If the math primitive doesn't exist yet,
// add it as a stdlib function first.
//
// Canonical form (third field) is reserved for the translation-hint
// stderr line that will land in a later commit; not used today.

#include "token.hpp"
#include <string>
#include <utility>
#include <vector>

namespace knot {

struct PhrasePattern {
    // Pattern words, interleaved with hole markers. Literal words are
    // matched against the source token's `.text` (so keyword tokens
    // like Tok::To with text "to" match a pattern word "to"). The
    // hole marker is the string "_"; holes greedily consume tokens up
    // to the next literal word, with bracket-depth tracking so commas
    // and parens inside a hole don't break the match.
    std::vector<std::string> words;

    // Function name to call with the holes as positional arguments.
    // Looked up in the normal lexical scope of the call site, so the
    // function has to exist as a builtin or a top-level `def`.
    std::string function;

    // Canonical surface form for the translation hint, e.g.
    // "sum(?)" -- "?" is replaced by the hole text. Reserved for a
    // later commit that prints these to stderr after each match.
    std::string canonical;

    // Zero-based indices of holes that should be wrapped as an
    // anonymous function `fn(x) -> HOLE` before being passed to the
    // call. The matched hole's free-variable analysis determines the
    // parameter name. Empty for the common case (all holes pass
    // through as raw expressions).
    std::vector<int> closure_holes;
};

// Names known to the runtime: every builtin registered in
// interpreter.hpp::register_builtins() plus every top-level def in
// stdlib/stdlib.knot. Used by free-variable analysis for closure-
// shaped phrase holes: any identifier in the hole that ISN'T in this
// set is treated as a candidate parameter for the wrapping fn(...).
//
// Keep in sync with the two source-of-truth lists. Over-including is
// safe (a missing free-var candidate just gets dropped); under-
// including merely produces a friendlier error when the parse fails.
inline bool is_known_fn_name(const std::string& name) {
    static const std::vector<std::string> kKnown = {
        // ---- C++ builtins (interpreter.hpp register_builtins) -----
        "input", "print", "panic", "plot",
        "at", "set", "append", "format",
        "num", "str", "len", "rows", "cols",
        "zeros", "ones", "eye", "dot", "norm",
        "matmul", "transpose",
        "sqrt", "abs", "sin", "cos", "exp", "log",
        "sort_vec",
        "rng_seed", "rng_uniform", "rng_normal",
        "read_csv", "write_csv",
        // ---- stdlib defs (stdlib/stdlib.knot) ---------------------
        "sum", "prod", "mean", "vmin", "vmax",
        "argmin", "argmax", "linspace", "arange",
        "reverse", "fill", "copy_vec", "copy_mat",
        "variance", "std", "floor_div", "median", "sort",
        "trace", "diag", "lu", "solve", "power_iter",
        "bisect", "newton", "newton_numeric",
        "trapezoid", "simpson", "rk4", "golden_section",
        "assert", "assert_msg", "assert_eq",
        "assert_near", "assert_mat_near",
    };
    for (const auto& k : kKnown) if (k == name) return true;
    return false;
}

// Pattern table. **Order matters**: more-specific patterns first, so
// `the L2 norm of _` is tried before `the norm of _`. Within a group,
// synonyms (e.g. average <-> mean, magnitude <-> norm) point at the
// same function.
inline const std::vector<PhrasePattern>& phrase_table() {
    static const std::vector<PhrasePattern> kTable = {
        // ---- Aggregates -----------------------------------------
        {{"the", "sum", "of", "_"},                       "sum",       "sum(?)"},
        {{"the", "product", "of", "_"},                   "prod",      "prod(?)"},
        {{"the", "mean", "of", "_"},                      "mean",      "mean(?)"},
        {{"the", "average", "of", "_"},                   "mean",      "mean(?)"},
        {{"the", "median", "of", "_"},                    "median",    "median(?)"},
        {{"the", "max", "of", "_"},                       "vmax",      "vmax(?)"},
        {{"the", "maximum", "of", "_"},                   "vmax",      "vmax(?)"},
        {{"the", "min", "of", "_"},                       "vmin",      "vmin(?)"},
        {{"the", "minimum", "of", "_"},                   "vmin",      "vmin(?)"},
        {{"the", "variance", "of", "_"},                  "variance",  "variance(?)"},
        {{"the", "standard", "deviation", "of", "_"},     "std",       "std(?)"},

        // ---- Matrix ops -----------------------------------------
        {{"the", "transpose", "of", "_"},                 "transpose", "transpose(?)"},
        {{"the", "trace", "of", "_"},                     "trace",     "trace(?)"},

        // ---- Vector ops (norm has multiple synonyms) ------------
        {{"the", "L2", "norm", "of", "_"},                "norm",      "norm(?)"},
        {{"the", "norm", "of", "_"},                      "norm",      "norm(?)"},
        {{"the", "magnitude", "of", "_"},                 "norm",      "norm(?)"},
        {{"the", "dot", "product", "of", "_", "and", "_"},   "dot",    "dot(?, ?)"},
        {{"the", "inner", "product", "of", "_", "and", "_"}, "dot",    "dot(?, ?)"},

        // ---- Indexing / size ------------------------------------
        {{"the", "size", "of", "_"},                      "len",       "len(?)"},
        {{"the", "length", "of", "_"},                    "len",       "len(?)"},
        {{"the", "number", "of", "rows", "of", "_"},      "rows",      "rows(?)"},
        {{"the", "number", "of", "columns", "of", "_"},   "cols",      "cols(?)"},

        // ---- Math primitives ------------------------------------
        {{"the", "sine", "of", "_"},                      "sin",       "sin(?)"},
        {{"the", "cosine", "of", "_"},                    "cos",       "cos(?)"},
        {{"the", "exponential", "of", "_"},               "exp",       "exp(?)"},
        {{"the", "natural", "log", "of", "_"},            "log",       "log(?)"},
        {{"the", "logarithm", "of", "_"},                 "log",       "log(?)"},
        {{"the", "square", "root", "of", "_"},            "sqrt",      "sqrt(?)"},
        {{"the", "absolute", "value", "of", "_"},         "abs",       "abs(?)"},

        // ---- Linear algebra -------------------------------------
        {{"the", "matrix", "product", "of", "_", "and", "_"}, "matmul", "matmul(?, ?)"},

        // ---- Closure-shaped phrases -----------------------------
        // The first hole in each of these is an EXPRESSION involving
        // a single free variable; parse_phrase wraps it as
        // fn(VAR) -> EXPR before passing to the call. The trailing
        // 4-arg form for `simpson` must come before the 3-arg form
        // so its longer literal tail wins the first-match-wins race.
        //
        // Pattern                                                                  Function       Canonical                       Closure holes
        {{"the", "integral", "of", "_", "from", "_", "to", "_", "with", "_", "intervals"},
            "simpson",        "simpson(?, ?, ?, ?)",         {0}},
        {{"the", "integral", "of", "_", "from", "_", "to", "_"},
            "simpson",        "simpson(?, ?, ?)",            {0}},
        {{"the", "root", "of", "_", "starting", "at", "_"},
            "newton_numeric", "newton_numeric(?, ?)",        {0}},
        {{"the", "root", "of", "_", "between", "_", "and", "_"},
            "bisect",         "bisect(?, ?, ?)",             {0}},
    };
    return kTable;
}

// True iff `word` is the hole marker.
inline bool is_hole(const std::string& word) { return word == "_"; }

// Try to match `pat` against `phrase_toks`. On success, fills
// `hole_slices` with the token ranges (start, end-exclusive) for each
// hole in left-to-right order, and returns true. On failure, returns
// false without mutating `hole_slices`.
//
// The matcher tracks bracket depth when scanning past holes, so a
// pattern like `the sum of _ and _` matched against tokens
// `the sum of f(a, b) and c` correctly puts `f(a, b)` in the first
// hole rather than mistaking the literal `and` inside the call.
inline bool try_match_phrase(const PhrasePattern& pat,
                             const std::vector<Token>& phrase_toks,
                             std::vector<std::pair<size_t, size_t>>& out_hole_slices) {
    std::vector<std::pair<size_t, size_t>> slices;
    size_t pi = 0;
    size_t ti = 0;
    while (pi < pat.words.size()) {
        const std::string& word = pat.words[pi];

        if (is_hole(word)) {
            // Find the next literal word in the pattern (if any). If
            // we're at the last hole with no trailing literal, we
            // gobble all remaining tokens.
            size_t next_literal_pi = pi + 1;
            while (next_literal_pi < pat.words.size()
                && is_hole(pat.words[next_literal_pi])) {
                // Two consecutive holes are ambiguous; reject. The
                // table doesn't currently produce these.
                return false;
            }

            size_t hole_start = ti;
            size_t hole_end;
            if (next_literal_pi == pat.words.size()) {
                // Last hole: take everything that remains.
                hole_end = phrase_toks.size();
                ti = hole_end;
                if (hole_end == hole_start) return false;  // empty hole
                slices.emplace_back(hole_start, hole_end);
                pi = next_literal_pi;
                continue;
            }

            // Scan forward for the next literal word, respecting
            // bracket depth so we don't mistake a delimiter inside a
            // parenthesized subexpression for the pattern's literal.
            const std::string& delim = pat.words[next_literal_pi];
            int depth = 0;
            size_t end = ti;
            bool found = false;
            while (end < phrase_toks.size()) {
                Tok k = phrase_toks[end].kind;
                if (k == Tok::LParen || k == Tok::LBracket) ++depth;
                else if (k == Tok::RParen || k == Tok::RBracket) {
                    if (depth > 0) --depth;
                }
                if (depth == 0 && phrase_toks[end].text == delim) {
                    found = true;
                    break;
                }
                ++end;
            }
            if (!found) return false;
            if (end == hole_start) return false;  // empty hole
            hole_end = end;
            slices.emplace_back(hole_start, hole_end);
            ti = end;
            pi = next_literal_pi;
            continue;
        }

        // Literal word. Must match the current token's `.text`
        // exactly. Keyword tokens (Tok::To, Tok::In, etc.) have their
        // source text preserved, so words like "to"/"in"/"as" work.
        if (ti >= phrase_toks.size()) return false;
        if (phrase_toks[ti].text != word) return false;
        ++ti;
        ++pi;
    }

    // All pattern words consumed -- the source tokens must also be
    // fully consumed for a complete match.
    if (ti != phrase_toks.size()) return false;

    out_hole_slices = std::move(slices);
    return true;
}

} // namespace knot
