// WASM entry point. Compiled to web/knot.js by `make wasm` via
// emscripten. Exposes a single C-callable function, knot_run(src),
// that takes a knot source string, executes it via the tree-walking
// interpreter (no --exec, no shelling out to cc), and returns the
// combined stdout + stderr as a heap-allocated null-terminated string
// the JS side must release with knot_free().
//
// Why an extra entry point: main.cpp drives the CLI -- arg parsing,
// file I/O, the various run modes. None of that is useful in a
// browser. This file is the smallest possible adapter: it loads the
// embedded stdlib, parses the user's source, runs it, captures the
// streams, hands the bytes back.
//
// The full interpreter (lexer, parser, AST, value system, builtins,
// stdlib) compiles into the WASM module unchanged. The CLI bits in
// main.cpp are deliberately not linked.

#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "render.hpp"
#include "stdlib_embedded.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace knot;

// Holds programs (the embedded stdlib and the user's source) for the
// duration of one knot_run call. Functions in the interpreter retain
// raw pointers into the program's statement bodies, so the program
// vector has to outlive the interpreter -- but only for the scope of
// the current run, not across calls.
static bool load_stdlib_into(Interpreter& interp, std::vector<std::vector<StmtPtr>>& retained) {
    std::string src = kStdlibSource;
    try {
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(toks);
        auto program = p.parse_program();
        interp.run(program);
        retained.push_back(std::move(program));
        return true;
    } catch (const Diag& d) {
        std::cerr << "stdlib load error:\n";
        render_diag("<stdlib>", src, d);
        return false;
    } catch (const std::exception& e) {
        std::cerr << "stdlib internal error: " << e.what() << "\n";
        return false;
    }
}

extern "C" {

// Run a knot source program. Returns a malloc'd, null-terminated buffer
// containing the run's combined stdout + stderr. The caller is
// responsible for releasing the buffer via knot_free().
//
// Errors (parse, runtime Diag, unexpected exception) are written into
// the same buffer using the same diagnostic format the CLI uses, so
// the browser side just renders one block of text either way.
//
// Returns nullptr only if allocation fails.
char* knot_run(const char* src_c) {
    if (!src_c) src_c = "";

    // Redirect stdout/stderr to a single stream for the duration of
    // this call. Saving the original rdbufs so concurrent stuff (e.g.
    // emscripten's own logging) keeps working between calls.
    std::stringstream captured;
    std::streambuf* old_cout = std::cout.rdbuf(captured.rdbuf());
    std::streambuf* old_cerr = std::cerr.rdbuf(captured.rdbuf());

    std::string src(src_c);
    std::vector<std::vector<StmtPtr>> retained;
    try {
        Interpreter interp;
        if (load_stdlib_into(interp, retained)) {
            Lexer lex(src);
            auto toks = lex.tokenize();
            Parser p(toks);
            auto program = p.parse_program();
            interp.run(program);
            retained.push_back(std::move(program));
        }
    } catch (const Diag& d) {
        // Source-position diagnostic with caret. <playground> is a
        // placeholder for the source filename, since the browser has no
        // path to display.
        render_diag("<playground>", src, d);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
    }

    // Restore the streams before we marshal the captured text out.
    std::cout.rdbuf(old_cout);
    std::cerr.rdbuf(old_cerr);

    std::string s = captured.str();
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

// Free a buffer previously returned by knot_run. Pairing knot_run with
// knot_free here (instead of asking the JS side to call free() on the
// emscripten heap) makes the contract obvious and lets us swap
// allocators in the future without breaking callers.
void knot_free(char* p) {
    std::free(p);
}

} // extern "C"
