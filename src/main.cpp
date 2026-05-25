#include "annotations.hpp"
#include "callgraph.hpp"
#include "codegen.hpp"
#include "hash.hpp"
#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "render.hpp"
#include "replay.hpp"
#include "stdlib_embedded.hpp"
#include "wasm_codegen.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

using namespace knot;

// Functions defined in user/stdlib code hold raw pointers into their parsed
// program (body, defaults). The program must outlive the interpreter. We keep
// programs in this owner vector so they're freed at process exit, not earlier.
static std::vector<std::vector<StmtPtr>> g_retained;

// Path of the knot binary itself (argv[0]). Set in main() and used by
// compile_and_run to locate src/runtime.h relative to the binary
// rather than the cwd, so `knot --exec` works from any directory.
static std::string g_argv0;

// Parse and execute the embedded stdlib source. Errors here are programmer
// errors in the stdlib itself, not the user's code — but we print them in
// the same format so they're at least debuggable.
static bool load_stdlib(Interpreter& interp) {
    std::string src = kStdlibSource;
    try {
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(toks, src);
        auto program = p.parse_program();
        interp.run(program);
        g_retained.push_back(std::move(program));
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

static int run_source(const std::string& filename, const std::string& src,
                      bool step_mode = false,
                      Annotations annots = Annotations{},
                      bool trap_nan = false,
                      const std::string& trace_path = "") {
    std::ofstream trace_file;
    if (!trace_path.empty()) {
        trace_file.open(trace_path);
        if (!trace_file) {
            std::cerr << "cannot open trace file " << trace_path << " for writing\n";
            return 1;
        }
    }
    try {
        Interpreter interp;
        if (!load_stdlib(interp)) return 1;
        if (step_mode) interp.enable_step_mode(std::move(annots));
        if (trap_nan)  interp.enable_trap_nan();
        if (!trace_path.empty()) interp.enable_record(trace_file);
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(toks, src);
        auto program = p.parse_program();
        interp.run(program);
        g_retained.push_back(std::move(program));
        return 0;
    } catch (const Diag& d) {
        render_diag(filename, src, d);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return 2;
    }
}

// Print one line per statement (recursively descending into function
// bodies and block stmts): HASH<TAB># preview-of-source. Used to seed
// a `.annot` file by hand or with an LLM.
static void dump_hashes_recursive(const std::vector<StmtPtr>& body,
                                  const std::vector<std::string>& lines,
                                  int depth) {
    std::string indent(depth * 2, ' ');
    for (const auto& s : body) {
        std::string h = stmt_hash(*s);
        std::string preview;
        int ln = s->span.line;
        if (ln >= 1 && ln <= (int)lines.size()) {
            preview = lines[(size_t)ln - 1];
            size_t k = 0;
            while (k < preview.size() && std::isspace((unsigned char)preview[k])) ++k;
            preview = preview.substr(k);
            if (preview.size() > 60) preview = preview.substr(0, 57) + "...";
        }
        std::cout << h << "\t# " << indent << "line " << ln << ": " << preview << "\n";
        // Recurse into sub-blocks.
        if (!s->body.empty())      dump_hashes_recursive(s->body, lines, depth + 1);
        if (!s->else_body.empty()) dump_hashes_recursive(s->else_body, lines, depth + 1);
    }
}

static int dump_hashes(const std::string& filename, const std::string& src) {
    try {
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(toks, src);
        auto program = p.parse_program();

        std::vector<std::string> lines;
        {
            std::string cur;
            for (char c : src) {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) lines.push_back(cur);
        }

        std::cout << "# Hashes for " << filename << "\n";
        std::cout << "# Format: HASH<TAB>DESCRIPTION (one per line).\n";
        std::cout << "# Composite keys for consecutive stmts at the same scope: HASH1+HASH2<TAB>DESCRIPTION.\n";
        std::cout << "# Lines starting with `#` are comments. Indentation in previews reflects nesting only.\n\n";
        dump_hashes_recursive(program, lines, 0);
        return 0;
    } catch (const Diag& d) {
        render_diag(filename, src, d);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return 2;
    }
}

// Helpers shared by --scaffold and --show.

namespace {

// Get the indented one-line preview of a stmt for human-readable comments.
std::string stmt_preview(const Stmt& s, const std::vector<std::string>& lines) {
    int ln = s.span.line;
    if (ln < 1 || ln > (int)lines.size()) return {};
    std::string p = lines[(size_t)ln - 1];
    size_t k = 0;
    while (k < p.size() && std::isspace((unsigned char)p[k])) ++k;
    p = p.substr(k);
    if (p.size() > 60) p = p.substr(0, 57) + "...";
    return p;
}

// Recursively walk stmts and emit scaffold lines. Composite annotations
// in `existing` are preserved as composite entries when all their hashes
// still match; single-stmt annotations carry over as singles. Statements
// without any matching annotation get a TODO placeholder.
void scaffold_recursive(const std::vector<StmtPtr>& body,
                        const std::vector<std::string>& lines,
                        const Annotations& existing,
                        int depth) {
    // First pass: compute hashes once.
    std::vector<std::string> hashes;
    hashes.reserve(body.size());
    for (const auto& s : body) hashes.push_back(stmt_hash(*s));

    size_t i = 0;
    while (i < body.size()) {
        // Try composite first, same lookup the runtime uses, so we preserve
        // composite groupings already in the existing .annot file.
        auto hit = existing.lookup(hashes, i);
        if (hit.consumed >= 2) {
            std::string key = hashes[i];
            for (int k = 1; k < hit.consumed; ++k) {
                key += "+";
                key += hashes[i + k];
            }
            std::cout << key << "\t" << hit.desc << "\n";
            // Recurse into the sub-blocks of every consumed stmt.
            for (int k = 0; k < hit.consumed; ++k) {
                if (!body[i + k]->body.empty())
                    scaffold_recursive(body[i + k]->body, lines, existing, depth + 1);
                if (!body[i + k]->else_body.empty())
                    scaffold_recursive(body[i + k]->else_body, lines, existing, depth + 1);
            }
            i += hit.consumed;
            continue;
        }

        // Single stmt: keep existing if present, else TODO.
        const std::string& h = hashes[i];
        std::string desc;
        auto it = existing.table.find(h);
        if (it != existing.table.end()) desc = it->second;
        else                            desc = "TODO: " + stmt_preview(*body[i], lines);
        std::cout << h << "\t" << desc << "\n";

        if (!body[i]->body.empty())
            scaffold_recursive(body[i]->body, lines, existing, depth + 1);
        if (!body[i]->else_body.empty())
            scaffold_recursive(body[i]->else_body, lines, existing, depth + 1);
        ++i;
    }
}

} // namespace

// `--scaffold FILE`: print a draft .annot file to stdout. If FILE.annot
// already exists, keep its descriptions and only add TODOs for stmts that
// don't have entries yet -- so re-scaffolding after edits preserves work.
static int scaffold(const std::string& filename, const std::string& src) {
    try {
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(toks, src);
        auto program = p.parse_program();

        // Split source into lines for previews.
        std::vector<std::string> lines;
        {
            std::string cur;
            for (char c : src) {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) lines.push_back(cur);
        }

        // Pick up any existing annotations so re-scaffolding is non-destructive.
        Annotations existing = load_annotations(std::string(filename) + ".annot");

        std::cout << "# Annotations for " << filename << "\n";
        std::cout << "# Edit the TODO placeholders to describe what each statement does.\n";
        std::cout << "# Format: HASH<TAB>DESCRIPTION.  Composite keys (HASH1+HASH2) group\n";
        std::cout << "# consecutive stmts under one description; see the README for details.\n";
        std::cout << "# A description starting with `*` will print every iteration (default\n";
        std::cout << "# is once per run).  Lines starting with `#` are comments.\n\n";
        scaffold_recursive(program, lines, existing, 0);
        return 0;
    } catch (const Diag& d) {
        render_diag(filename, src, d);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return 2;
    }
}

// Helper for --show: collect (line_number, indent, description) tuples
// for every stmt that has an annotation. We use line_number to insert
// comments into the source listing later.
namespace {

struct ShowEntry {
    int line;
    int depth;     // for indenting the comment to match nesting
    std::string desc;
    int consumed;  // 1 for single stmt, >1 for composite (informational)
};

// Walk the body and find the longest composite annotation that starts at
// each position, recording it.  Then recurse into sub-blocks.
void collect_show(const std::vector<StmtPtr>& body,
                  const Annotations& annots,
                  int depth,
                  std::vector<ShowEntry>& out) {
    std::vector<std::string> hashes;
    hashes.reserve(body.size());
    for (const auto& s : body) hashes.push_back(stmt_hash(*s));

    size_t i = 0;
    while (i < body.size()) {
        auto hit = annots.lookup(hashes, i);
        if (hit.consumed > 0) {
            ShowEntry e;
            e.line = body[i]->span.line;
            e.depth = depth;
            e.desc = hit.desc;
            e.consumed = hit.consumed;
            out.push_back(std::move(e));
            // Recurse into all consumed stmts' sub-blocks.
            for (int k = 0; k < hit.consumed; ++k) {
                if (!body[i + k]->body.empty())
                    collect_show(body[i + k]->body, annots, depth + 1, out);
                if (!body[i + k]->else_body.empty())
                    collect_show(body[i + k]->else_body, annots, depth + 1, out);
            }
            i += hit.consumed;
        } else {
            if (!body[i]->body.empty())
                collect_show(body[i]->body, annots, depth + 1, out);
            if (!body[i]->else_body.empty())
                collect_show(body[i]->else_body, annots, depth + 1, out);
            ++i;
        }
    }
}

} // namespace

// `--show FILE`: print the source file with annotations interleaved as
// `# DESC` comments inserted above each annotated stmt's line. The original
// source is preserved verbatim; comments are inserted. This is the read-only
// view that makes the annotation system human-usable: no hash matching.
static int show(const std::string& filename, const std::string& src) {
    try {
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(toks, src);
        auto program = p.parse_program();

        Annotations annots = load_annotations(std::string(filename) + ".annot");
        if (annots.empty()) {
            std::cerr << "(no annotations found at " << filename << ".annot)\n";
        }

        std::vector<ShowEntry> entries;
        collect_show(program, annots, 0, entries);

        // Group entries by line so multiple annotations on the same line
        // (rare but possible) print together.
        std::unordered_map<int, std::vector<ShowEntry>> by_line;
        for (auto& e : entries) by_line[e.line].push_back(std::move(e));

        // Split source into lines.
        std::vector<std::string> lines;
        {
            std::string cur;
            for (char c : src) {
                if (c == '\n') { lines.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) lines.push_back(cur);
        }

        // Walk source lines, emitting annotation comments above each
        // annotated line. We match the leading whitespace of the source
        // line so the comment indents naturally with the code.
        for (size_t i = 0; i < lines.size(); ++i) {
            int line_no = (int)i + 1;
            auto it = by_line.find(line_no);
            if (it != by_line.end()) {
                const std::string& src_line = lines[i];
                // Capture indent of the source line so the comment lines up.
                std::string indent;
                for (char c : src_line) {
                    if (c == ' ' || c == '\t') indent += c;
                    else break;
                }
                for (const auto& e : it->second) {
                    std::string desc = e.desc;
                    // Strip a leading '*' since it has no effect on --show.
                    if (!desc.empty() && desc[0] == '*') desc = desc.substr(1);
                    std::string suffix;
                    if (e.consumed > 1) {
                        suffix = " (covers next " + std::to_string(e.consumed) + " stmts)";
                    }
                    std::cout << indent << "# " << desc << suffix << "\n";
                }
            }
            std::cout << lines[i] << "\n";
        }
        return 0;
    } catch (const Diag& d) {
        render_diag(filename, src, d);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return 2;
    }
}

static int repl() {
    Interpreter interp;
    if (!load_stdlib(interp)) return 1;
    std::cout << "knot repl. ctrl-d to exit.  type `lslib` to list the standard library, `whatis NAME` for help.\n";
    std::string line;
    int line_num = 1;
    while (true) {
        std::cout << ">>> " << std::flush;
        if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }
        if (line.empty()) continue;

        // REPL-only meta commands. Intercepted before parsing.
        if (line == "lslib") {
            // Walk globals, list functions, print one-line summary.
            EnvPtr g = interp.global_env();
            std::vector<std::pair<std::string, std::string>> rows;
            for (const auto& kv : g->bindings()) {
                if (!kv.second.is_fn()) continue;
                const std::string& doc = kv.second.as_fn().docstring;
                std::string summary;
                if (!doc.empty()) {
                    // First line, trimmed.
                    size_t nl = doc.find('\n');
                    summary = doc.substr(0, nl == std::string::npos ? doc.size() : nl);
                    // Strip leading/trailing whitespace.
                    while (!summary.empty() && std::isspace((unsigned char)summary.front())) summary.erase(summary.begin());
                    while (!summary.empty() && std::isspace((unsigned char)summary.back())) summary.pop_back();
                }
                rows.emplace_back(kv.first, summary);
            }
            std::sort(rows.begin(), rows.end());
            size_t name_w = 0;
            for (auto& r : rows) if (r.first.size() > name_w) name_w = r.first.size();
            for (auto& r : rows) {
                std::cout << "  " << r.first;
                for (size_t i = r.first.size(); i < name_w + 2; ++i) std::cout << ' ';
                std::cout << r.second << "\n";
            }
            continue;
        }
        if (line.rfind("whatis ", 0) == 0) {
            std::string name = line.substr(7);
            while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
            while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
            EnvPtr g = interp.global_env();
            try {
                Value v = g->get(name);
                if (v.is_fn()) {
                    const Function& f = v.as_fn();
                    std::cout << f.name << "(";
                    for (size_t i = 0; i < f.params.size(); ++i) {
                        if (i) std::cout << ", ";
                        std::cout << f.params[i];
                    }
                    std::cout << ")\n";
                    if (!f.docstring.empty()) std::cout << "  " << f.docstring << "\n";
                    else std::cout << "  (no docstring)\n";
                } else {
                    std::cout << name << " : " << v.type_name() << "\n";
                }
            } catch (...) {
                std::cout << "no binding named " << name << "\n";
            }
            continue;
        }

        std::string filename = "<repl line " + std::to_string(line_num++) + ">";

        auto try_parse_and_run = [&](const std::string& src) -> bool {
            try {
                Lexer lex(src);
                auto toks = lex.tokenize();
                Parser p(toks, src);
                auto program = p.parse_program();
                interp.run(program);
                g_retained.push_back(std::move(program));
                return true;
            } catch (const Diag& d) {
                render_diag(filename, src, d);
                return false;
            } catch (const std::exception& e) {
                std::cerr << "internal error: " << e.what() << "\n";
                return false;
            }
        };

        // Heuristic: if the line starts with a statement keyword or '{',
        // run it as-is. Otherwise wrap as `print(expr)` for interactivity.
        auto starts_with_kw = [&](const std::string& s, const char* kw) {
            size_t n = std::strlen(kw);
            if (s.size() < n) return false;
            for (size_t i = 0; i < n; ++i) if (s[i] != kw[i]) return false;
            if (s.size() == n) return true;
            char c = s[n];
            return !(std::isalnum((unsigned char)c) || c == '_');
        };

        bool looks_like_stmt =
            starts_with_kw(line, "def") || starts_with_kw(line, "if")
         || starts_with_kw(line, "while") || starts_with_kw(line, "loop")
         || starts_with_kw(line, "return")
         || line.front() == '{';

        if (!looks_like_stmt) {
            std::string wrapped = "print(" + line + ")\n";
            try {
                Lexer lex(wrapped);
                auto toks = lex.tokenize();
                Parser p(toks, wrapped);
                auto program = p.parse_program();
                interp.run(program);
                g_retained.push_back(std::move(program));
                continue;
            } catch (const Diag&) {
            } catch (const std::exception&) {
            }
        }

        try_parse_and_run(line + "\n");
    }
    return 0;
}

// Generate C from a knot source file. Writes the C to `out_path` and
// returns 0 on success. Loads the embedded stdlib into the codegen context
// so user code can call stdlib functions.
static int transpile_to_c(const std::string& filename, const std::string& src,
                          const std::string& out_path) {
    try {
        // First, parse the stdlib so its functions are visible during codegen.
        std::string stdlib_src = kStdlibSource;
        Lexer slex(stdlib_src);
        auto stoks = slex.tokenize();
        Parser sp(stoks, stdlib_src);
        auto stdlib_program = sp.parse_program();

        // Then parse the user source.
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser p(toks, src);
        auto program = p.parse_program();

        // Concatenate: stdlib first, then user code. Codegen treats this
        // as one program; stdlib functions become C functions that user
        // code can call.
        std::vector<StmtPtr> combined;
        for (auto& s : stdlib_program) combined.push_back(std::move(s));
        for (auto& s : program)        combined.push_back(std::move(s));

        Codegen cg;
        std::string c = cg.generate(combined, filename);
        std::ofstream out(out_path);
        out << c;
        return 0;
    } catch (const Diag& d) {
        render_diag(filename, src, d);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return 2;
    }
}

// Compile-and-run with a source-hash cache. If the binary in /tmp matches
// the current source hash, skip compilation entirely and just run it. On
// repeat runs of unchanged source, --exec drops from ~50ms (cc invocation)
// to ~3ms (just spawn the binary).
static int compile_and_run(const std::string& filename, const std::string& src) {
    // Derive a stable name for the generated artifacts.
    std::string base = filename;
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::string c_path    = "/tmp/knot_" + base + ".c";
    std::string bin_path  = "/tmp/knot_" + base + ".out";
    std::string hash_path = "/tmp/knot_" + base + ".srchash";

    // Compute a hash that mixes the user source with the runtime header,
    // so updating runtime.h busts the cache too.
    std::string runtime_src;
    {
        // Same lookup logic as below; do it once up-front so we can hash.
        std::vector<std::string> candidates = {
            "src/runtime.h",
            "./runtime.h",
        };
        if (!g_argv0.empty()) {
            size_t slash = g_argv0.find_last_of('/');
            std::string bin_dir = (slash == std::string::npos)
                ? std::string(".")
                : g_argv0.substr(0, slash);
            candidates.push_back(bin_dir + "/src/runtime.h");
            candidates.push_back(bin_dir + "/../src/runtime.h");
            candidates.push_back(bin_dir + "/runtime.h");
        }
        candidates.push_back("/home/claude/knot/src/runtime.h");
        for (const auto& p : candidates) {
            std::ifstream test(p);
            if (test) {
                std::stringstream ss; ss << test.rdbuf();
                runtime_src = ss.str();
                break;
            }
        }
    }
    uint64_t h = fnv1a_64(src);
    // Mix in the runtime so changes to it bust the cache.
    h ^= fnv1a_64(runtime_src) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    std::string current_hash = hash_hex(h);

    // Cache check: do the binary and hash file exist, and does the cached
    // hash match? If yes, skip codegen and compile.
    {
        std::ifstream hf(hash_path);
        std::ifstream bf(bin_path);
        if (hf && bf) {
            std::string cached_hash;
            std::getline(hf, cached_hash);
            if (cached_hash == current_hash) {
                return std::system(bin_path.c_str());
            }
        }
    }

    int rc = transpile_to_c(filename, src, c_path);
    if (rc != 0) return rc;

    // Find runtime.h directory for the -I flag. We try, in order:
    //   1. ./src/runtime.h        (running from the repo root)
    //   2. ./runtime.h            (legacy)
    //   3. <bin_dir>/src/runtime.h  (sibling-of-the-binary layout)
    //   4. <bin_dir>/../src/runtime.h  (binary in a build/ subdir)
    //   5. /home/claude/knot/src/runtime.h  (legacy sandbox path)
    // The bin_dir candidates let `knot --exec` work from any cwd.
    std::vector<std::string> candidates = {
        "src/runtime.h",
        "./runtime.h",
    };
    if (!g_argv0.empty()) {
        size_t slash = g_argv0.find_last_of('/');
        std::string bin_dir = (slash == std::string::npos)
            ? std::string(".")
            : g_argv0.substr(0, slash);
        candidates.push_back(bin_dir + "/src/runtime.h");
        candidates.push_back(bin_dir + "/../src/runtime.h");
        candidates.push_back(bin_dir + "/runtime.h");
    }
    candidates.push_back("/home/claude/knot/src/runtime.h");

    std::string runtime_inc;
    for (const auto& p : candidates) {
        std::ifstream test(p);
        if (test) {
            runtime_inc = p;
            size_t s = runtime_inc.find_last_of('/');
            runtime_inc = (s == std::string::npos) ? "." : runtime_inc.substr(0, s);
            break;
        }
    }
    if (runtime_inc.empty()) {
        std::cerr << "could not locate runtime.h\n";
        return 1;
    }

    // Build the C++ runtime extension once and cache the .o.  We invoke c++
    // (which pulls in libstdc++) as the linker driver so the .o's stdlib
    // symbols resolve.  Since runtime_ext.cpp doesn't change between knot
    // runs, we only rebuild it if the .o is missing.
    std::string ext_o = "/tmp/knot_runtime_ext.o";
    std::string ext_cpp = runtime_inc + "/runtime_ext.cpp";
    {
        std::ifstream o_test(ext_o);
        std::ifstream cpp_test(ext_cpp);
        bool need_build = !o_test || !cpp_test;
        if (!need_build) {
            // Rebuild if the .cpp is newer than the .o (simple mtime check).
            struct stat o_st, cpp_st;
            if (stat(ext_o.c_str(), &o_st) == 0
             && stat(ext_cpp.c_str(), &cpp_st) == 0) {
                if (cpp_st.st_mtime > o_st.st_mtime) need_build = true;
            }
        }
        if (need_build && cpp_test) {
            std::string build = "c++ -O2 -std=c++17 -c -I" + runtime_inc
                              + " " + ext_cpp + " -o " + ext_o;
            if (std::system(build.c_str()) != 0) {
                std::cerr << "failed to build runtime extension\n";
                return 1;
            }
        }
    }

    // Use c++ as the linker driver so libstdc++ comes in for the extension.
    // The user's transpiled code itself is plain C; the .o brings in the
    // sort/random/csv functions.  `-x c` is scoped to just the .c file by
    // following it with `-x none` so the .o isn't misinterpreted.
    std::string cc_cmd = "c++ -O2 -I" + runtime_inc
                       + " -x c " + c_path + " -x none "
                       + ext_o + " -lstdc++ -lm -o " + bin_path;
    int cc_rc = std::system(cc_cmd.c_str());
    if (cc_rc != 0) {
        std::cerr << "C compilation failed\n";
        std::cerr << "(generated source at " << c_path << ")\n";
        return 1;
    }

    // Persist the hash so the next run can skip compile.
    {
        std::ofstream hf(hash_path);
        hf << current_hash << "\n";
    }
    return std::system(bin_path.c_str());
}

int main(int argc, char** argv) {
    g_argv0 = argv[0];
    // CLI parsing.
    //   knot                     -> REPL
    //   knot FILE                -> run FILE
    //   knot --step FILE         -> run FILE with sidecar annotations
    //   knot --hashes FILE       -> raw HASH<tab>preview dump
    //   knot --scaffold FILE     -> emit a .annot draft to stdout
    //   knot --show FILE         -> print source with annotations inlined
    //   knot --cc FILE           -> transpile to C; print to stdout
    //   knot --exec FILE         -> transpile, compile with cc, run binary
    enum class Mode { Run, Step, Hashes, Scaffold, Show, CC, Exec, Callgraph, Replay, Test, Fuzz, WasmTest } mode = Mode::Run;
    const char* filename = nullptr;
    bool trap_nan = false;
    std::string trace_path;
    std::string fuzz_slot;                  // --fuzz NAME=ALT1,ALT2,... (the NAME)
    std::vector<std::string> fuzz_alts;     // (the ALTs)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--step")     mode = Mode::Step;
        else if (arg == "--hashes")   mode = Mode::Hashes;
        else if (arg == "--scaffold") mode = Mode::Scaffold;
        else if (arg == "--show")     mode = Mode::Show;
        else if (arg == "--cc")       mode = Mode::CC;
        else if (arg == "--exec")     mode = Mode::Exec;
        else if (arg == "--callgraph") mode = Mode::Callgraph;
        else if (arg == "--replay")    mode = Mode::Replay;
        else if (arg == "--test")      mode = Mode::Test;
        else if (arg == "--wasm-test") mode = Mode::WasmTest;
        else if (arg == "--trap-nan") trap_nan = true;
        else if (arg == "--no-hints")  phrase_hints_enabled() = false;
        else if (arg == "--no-narrate") narration_enabled() = false;
        else if (arg == "--narrate-step") narration_step_enabled() = true;
        else if (arg.rfind("--fuzz=", 0) == 0
             || (arg == "--fuzz" && i + 1 < argc)) {
            // Both `--fuzz NAME=ALT,...` and `--fuzz=NAME=ALT,...`.
            std::string spec = (arg.rfind("--fuzz=", 0) == 0)
                ? arg.substr(7)
                : argv[++i];
            size_t eq = spec.find('=');
            if (eq == std::string::npos) {
                std::cerr << "--fuzz expects NAME=ALT1[,ALT2,...]\n";
                return 2;
            }
            fuzz_slot = spec.substr(0, eq);
            std::string rest = spec.substr(eq + 1);
            // Split on commas.
            size_t s = 0;
            while (s < rest.size()) {
                size_t c = rest.find(',', s);
                if (c == std::string::npos) c = rest.size();
                fuzz_alts.push_back(rest.substr(s, c - s));
                s = c + 1;
            }
            mode = Mode::Fuzz;
        }
        else if (arg == "--record") {
            // Trace path is filled in below once we know the input filename.
            trace_path = "__placeholder__";
        }
        else if (!arg.empty() && arg[0] != '-' && !filename) filename = argv[i];
        else { std::cerr << "unrecognized arg: " << arg << "\n"; return 2; }
    }
    if (trace_path == "__placeholder__") {
        if (!filename) {
            std::cerr << "--record needs an input file\n";
            return 2;
        }
        trace_path = std::string(filename) + ".trace";
    }
    if (!filename) return repl();

    // --callgraph and --replay both read a .trace file (produced by
    // --record), not a .knot source. Handle them before opening as source.
    if (mode == Mode::Callgraph) {
        try {
            std::string text = read_file(filename);
            CallGraph g = parse_trace(text);
            std::cout << render_dot(g);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "callgraph: " << e.what() << "\n";
            return 1;
        }
    }
    if (mode == Mode::Replay) {
        try {
            std::string text = read_file(filename);
            ReplayTrace rt = parse_replay_trace(text);
            return run_replay_repl(rt);
        } catch (const std::exception& e) {
            std::cerr << "replay: " << e.what() << "\n";
            return 1;
        }
    }
    if (mode == Mode::WasmTest) {
        // Emit a stub WASM module to FILENAME for end-to-end testing
        // of the binary writer + browser loader. The module exports
        // `main` of type () -> f64 returning 42.0; instantiated in JS
        // it should produce 42.
        WasmCodegen cg;
        std::vector<uint8_t> bytes = cg.compile_test();
        std::ofstream out(filename, std::ios::binary);
        if (!out) { std::cerr << "cannot open " << filename << " for writing\n"; return 1; }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  (std::streamsize)bytes.size());
        std::cerr << "wrote " << bytes.size() << " bytes to " << filename << "\n";
        return 0;
    }

    std::ifstream f(filename);
    if (!f) { std::cerr << "cannot open " << filename << "\n"; return 1; }
    std::stringstream ss; ss << f.rdbuf();
    std::string src = ss.str();

    switch (mode) {
        case Mode::Hashes:   return dump_hashes(filename, src);
        case Mode::Scaffold: return scaffold(filename, src);
        case Mode::Show:     return show(filename, src);
        case Mode::WasmTest: return 0; // handled above
        case Mode::Step: {
            Annotations annots = load_annotations(std::string(filename) + ".annot");
            if (annots.empty()) {
                std::cerr << "(no annotations found at " << filename << ".annot)\n";
            }
            return run_source(filename, src, true, std::move(annots), trap_nan, trace_path);
        }
        case Mode::CC: {
            // Print generated C to stdout. Stdlib gets prepended.
            try {
                std::string stdlib_src = kStdlibSource;
                Lexer slex(stdlib_src);
                auto stoks = slex.tokenize();
                Parser sp(stoks, stdlib_src);
                auto stdlib_program = sp.parse_program();

                Lexer lex(src);
                auto toks = lex.tokenize();
                Parser p(toks, src);
                auto program = p.parse_program();

                std::vector<StmtPtr> combined;
                for (auto& s : stdlib_program) combined.push_back(std::move(s));
                for (auto& s : program)        combined.push_back(std::move(s));

                Codegen cg;
                std::cout << cg.generate(combined, filename);
                return 0;
            } catch (const Diag& d) {
                render_diag(filename, src, d);
                return 1;
            } catch (const std::exception& e) {
                std::cerr << "internal error: " << e.what() << "\n";
                return 2;
            }
        }
        case Mode::Exec:
            return compile_and_run(filename, src);
        case Mode::Run:
            return run_source(filename, src, false, Annotations{}, trap_nan, trace_path);
        case Mode::Test: {
            // Run all top-level `test "name" { ... }` blocks. Non-test
            // top-level stmts execute once first (to set up defs / globals).
            // Returns 1 if any test failed.
            try {
                Interpreter interp;
                if (!load_stdlib(interp)) return 1;
                Lexer lex(src);
                auto toks = lex.tokenize();
                Parser p(toks, src);
                auto program = p.parse_program();
                int failed = interp.run_tests(program, filename);
                g_retained.push_back(std::move(program));
                return failed == 0 ? 0 : 1;
            } catch (const Diag& d) {
                render_diag(filename, src, d);
                return 1;
            } catch (const std::exception& e) {
                std::cerr << "internal error: " << e.what() << "\n";
                return 2;
            }
        }
        case Mode::Fuzz: {
            // Run the file once per alternative. After each run capture
            // the program's stdout, then print all outputs and report
            // whether they agree. Disagreement is the diagnostic signal
            // that one of the methods is wrong (or has different
            // numerical character than the others).
            //
            // Each run is independent: fresh Interpreter, fresh stdlib
            // load, fresh parse of the same source. The parse is
            // re-done because Function values carry raw pointers to
            // the AST and the prior run's program owns its own copy.
            std::vector<std::string> outputs;
            for (const std::string& alt : fuzz_alts) {
                std::ostringstream captured;
                std::streambuf* old_cout = std::cout.rdbuf(captured.rdbuf());
                try {
                    Interpreter interp;
                    if (!load_stdlib(interp)) {
                        std::cout.rdbuf(old_cout);
                        return 1;
                    }
                    Lexer lex(src);
                    auto toks = lex.tokenize();
                    Parser p(toks, src);
                    auto program = p.parse_program();
                    interp.run_with_fuzz(program, fuzz_slot, alt);
                    g_retained.push_back(std::move(program));
                } catch (const Diag& d) {
                    std::cout.rdbuf(old_cout);
                    std::cerr << "fuzz run [" << alt << "] failed:\n";
                    render_diag(filename, src, d);
                    return 1;
                } catch (const std::exception& e) {
                    std::cout.rdbuf(old_cout);
                    std::cerr << "fuzz run [" << alt << "] internal error: "
                              << e.what() << "\n";
                    return 2;
                }
                std::cout.rdbuf(old_cout);
                outputs.push_back(captured.str());
            }

            // Print each run's output.
            std::cout << "== fuzz: " << fuzz_alts.size()
                      << " methods over `" << fuzz_slot << "` ==\n";
            for (size_t i = 0; i < fuzz_alts.size(); ++i) {
                std::cout << "\n  [" << fuzz_alts[i] << "]\n";
                std::istringstream lines(outputs[i]);
                std::string line;
                while (std::getline(lines, line)) {
                    std::cout << "    " << line << "\n";
                }
            }

            // Agreement check: all outputs identical?
            std::cout << "\n";
            bool agree = true;
            for (size_t i = 1; i < outputs.size(); ++i) {
                if (outputs[i] != outputs[0]) { agree = false; break; }
            }
            if (agree) {
                std::cout << "  AGREE: all " << fuzz_alts.size()
                          << " methods produced identical output.\n";
                return 0;
            } else {
                std::cout << "  DISAGREEMENT: outputs differ across methods.\n";
                return 1;
            }
        }
        case Mode::Callgraph: return 0; // handled above; unreachable
        case Mode::Replay:    return 0; // handled above; unreachable
    }
    return 0;
}

