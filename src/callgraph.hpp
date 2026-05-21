#pragma once
// Read a --record trace file and render the dynamic call graph as
// Graphviz .dot. Nodes are functions, edges are caller -> callee with a
// label showing call count and the first-seen argument tuple.
//
// Edges attributed by maintaining a stack of currently-executing
// functions: at each CALL line, the top-of-stack is the caller. The
// top-level program uses the sentinel "<top>" as its function name.
//
// The trace parser is intentionally tolerant: a mismatched RET (function
// name doesn't match top-of-stack) prints a warning to stderr and
// continues. An unbalanced CALL (no matching RET, e.g. the program
// crashed inside the function) leaves that function on the stack at end
// of trace; we attribute remaining edges normally.

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace knot {

struct CallGraphEdge {
    int count = 0;
    std::string first_args; // formatted args from the first call on this edge
};

struct CallGraph {
    std::set<std::string> nodes;
    // Keyed by (caller, callee). std::map for deterministic .dot output.
    std::map<std::pair<std::string, std::string>, CallGraphEdge> edges;
};

// Parse one CALL line: extract function name and args text.
// Input like:  CALL hypot(3, 4) at line=10:7
// Returns true on success and fills *name, *args_text.
inline bool parse_call_line(const std::string& line,
                            std::string* name,
                            std::string* args_text) {
    // line starts with "CALL "
    if (line.compare(0, 5, "CALL ") != 0) return false;
    size_t paren = line.find('(', 5);
    if (paren == std::string::npos) return false;
    // The args list ends at the matching close-paren just before " at line=".
    size_t at_marker = line.rfind(") at line=");
    if (at_marker == std::string::npos || at_marker < paren) return false;
    *name = line.substr(5, paren - 5);
    *args_text = line.substr(paren + 1, at_marker - paren - 1);
    return true;
}

// Parse one RET line: RET <name> -> <value>
inline bool parse_ret_line(const std::string& line, std::string* name) {
    if (line.compare(0, 4, "RET ") != 0) return false;
    size_t arrow = line.find(" -> ", 4);
    if (arrow == std::string::npos) return false;
    *name = line.substr(4, arrow - 4);
    return true;
}

inline CallGraph parse_trace(const std::string& text) {
    CallGraph g;
    std::vector<std::string> stack; // currently-executing functions
    stack.push_back("<top>");
    g.nodes.insert("<top>");

    std::istringstream iss(text);
    std::string line;
    int line_no = 0;
    while (std::getline(iss, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#' || line[0] == ' ') continue;

        std::string name, args;
        if (parse_call_line(line, &name, &args)) {
            g.nodes.insert(name);
            const std::string& caller = stack.back();
            auto& edge = g.edges[{caller, name}];
            if (edge.count == 0) edge.first_args = args;
            edge.count += 1;
            stack.push_back(name);
            continue;
        }
        if (parse_ret_line(line, &name)) {
            // Tolerate mismatched returns: warn and pop best-effort.
            if (stack.size() <= 1) {
                std::cerr << "callgraph: RET " << name
                          << " with empty call stack (trace line " << line_no
                          << ")\n";
                continue;
            }
            if (stack.back() != name) {
                std::cerr << "callgraph: RET " << name << " but top of stack is "
                          << stack.back() << " (trace line " << line_no << ")\n";
            }
            stack.pop_back();
            continue;
        }
        // STEP lines and anything else: ignore.
    }
    return g;
}

// Escape a string for use inside a Graphviz double-quoted label.
inline std::string dot_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else out += c;
    }
    return out;
}

// Truncate long arg lists so .dot labels don't explode (a 1000-element
// vec literal makes Graphviz unhappy and unreadable anyway).
inline std::string truncate_for_label(const std::string& s, size_t max_len = 60) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len - 3) + "...";
}

inline std::string render_dot(const CallGraph& g) {
    std::ostringstream out;
    out << "digraph callgraph {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box, fontname=\"monospace\"];\n";
    out << "  edge [fontname=\"monospace\", fontsize=10];\n";
    for (const auto& n : g.nodes) {
        out << "  \"" << dot_escape(n) << "\";\n";
    }
    for (const auto& [pair, edge] : g.edges) {
        out << "  \"" << dot_escape(pair.first) << "\" -> \""
            << dot_escape(pair.second) << "\""
            << " [label=\"x" << edge.count;
        if (!edge.first_args.empty()) {
            out << "\\n(" << dot_escape(truncate_for_label(edge.first_args)) << ")";
        }
        out << "\"];\n";
    }
    out << "}\n";
    return out.str();
}

inline std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open trace file: " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace knot
