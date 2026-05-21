#pragma once
// --replay: load a .trace file (produced by --record) and drop into an
// interactive prompt where you can scrub through execution one STEP at
// a time, inspect any variable's value at any point, see its history,
// or find the first step where it took a particular value.
//
// This is the user-facing piece of the omniscient-debugger pitch. The
// trace is the substrate; replay is the verb.
//
// Commands (`help` prints them too):
//   next, n            advance one step
//   prev, p            back one step
//   goto N             jump to step N
//   list, l            show all variables at the current step
//   inspect VAR, i VAR show one variable's value at the current step
//   history VAR, h VAR show every step where VAR was set/changed
//   calls              show CALL/RET events between current and next step
//   find STR           find first step whose state contains STR; jump to it
//   help, ?            command list
//   quit, q            exit
//
// v1 limitations: variables are looked up by literal name only -- no
// indexing or arithmetic in the expression yet. Function-local scopes
// aren't in the trace (globals only) so locals inside a function call
// are invisible. Both are deliberate v1 cuts.

#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace knot {

struct ReplayStep {
    int index = -1;
    int line = 0, col = 0;
    // (name, formatted_value) in sorted order, matching the trace.
    std::vector<std::pair<std::string, std::string>> vars;
};

struct ReplayCall {
    bool is_return = false;       // CALL when false, RET when true
    std::string fn;
    std::string args_or_value;    // args for CALL, returned value for RET
    int line = 0, col = 0;        // call site (CALL only)
    int after_step = -1;          // index of last STEP before this event
};

struct ReplayTrace {
    std::vector<ReplayStep> steps;
    std::vector<ReplayCall> calls;
};

inline void trim_inplace(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
}

inline ReplayTrace parse_replay_trace(const std::string& text) {
    ReplayTrace t;
    int last_step_index = -1;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        trim_inplace(line);
        if (line.empty() || line[0] == '#') continue;

        // STEP block header: "STEP <n> line=<L>:<C>"
        if (line.compare(0, 5, "STEP ") == 0) {
            ReplayStep s;
            // Parse N
            size_t p = 5;
            size_t e1 = line.find(' ', p);
            if (e1 == std::string::npos) continue;
            s.index = std::stoi(line.substr(p, e1 - p));
            // Parse line=L:C
            size_t lp = line.find("line=");
            if (lp != std::string::npos) {
                lp += 5;
                size_t colon = line.find(':', lp);
                if (colon != std::string::npos) {
                    s.line = std::stoi(line.substr(lp, colon - lp));
                    s.col  = std::stoi(line.substr(colon + 1));
                }
            }
            t.steps.push_back(std::move(s));
            last_step_index = (int)t.steps.size() - 1;
            continue;
        }
        // Indented variable line: "  name = value"
        if (line.size() >= 2 && line[0] == ' ' && line[1] == ' ') {
            if (last_step_index < 0) continue;
            std::string body = line.substr(2);
            size_t eq = body.find(" = ");
            if (eq == std::string::npos) continue;
            std::string name = body.substr(0, eq);
            std::string value = body.substr(eq + 3);
            t.steps[last_step_index].vars.emplace_back(std::move(name), std::move(value));
            continue;
        }
        // CALL <name>(<args>) at line=<L>:<C>
        if (line.compare(0, 5, "CALL ") == 0) {
            ReplayCall c;
            c.is_return = false;
            size_t paren = line.find('(', 5);
            size_t at_marker = line.rfind(") at line=");
            if (paren == std::string::npos || at_marker == std::string::npos
             || at_marker < paren) continue;
            c.fn = line.substr(5, paren - 5);
            c.args_or_value = line.substr(paren + 1, at_marker - paren - 1);
            size_t lp = at_marker + 10; // skip ") at line="
            size_t colon = line.find(':', lp);
            if (colon != std::string::npos) {
                c.line = std::stoi(line.substr(lp, colon - lp));
                c.col  = std::stoi(line.substr(colon + 1));
            }
            c.after_step = last_step_index;
            t.calls.push_back(std::move(c));
            continue;
        }
        // RET <name> -> <value>
        if (line.compare(0, 4, "RET ") == 0) {
            ReplayCall c;
            c.is_return = true;
            size_t arrow = line.find(" -> ", 4);
            if (arrow == std::string::npos) continue;
            c.fn = line.substr(4, arrow - 4);
            c.args_or_value = line.substr(arrow + 4);
            c.after_step = last_step_index;
            t.calls.push_back(std::move(c));
            continue;
        }
    }
    return t;
}

// Find the value of `name` at step `step_idx`, returning empty if absent.
inline std::string lookup_var(const ReplayStep& s, const std::string& name) {
    for (const auto& [n, v] : s.vars)
        if (n == name) return v;
    return "";
}

// Split a command line into whitespace-separated tokens. First token is
// the command name; the remainder may be a single argument (we preserve
// embedded spaces by joining all non-first tokens back together with " ").
inline std::vector<std::string> split_cmd(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        if (i >= line.size()) break;
        size_t j = i;
        while (j < line.size() && !std::isspace((unsigned char)line[j])) ++j;
        out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

inline void print_help() {
    std::cout
        << "  next, n            advance one step\n"
        << "  prev, p            back one step\n"
        << "  goto N             jump to step N (0-indexed)\n"
        << "  list, l            show all variables at the current step\n"
        << "  inspect VAR, i VAR show one variable at the current step\n"
        << "  history VAR, h VAR show every step where VAR changed\n"
        << "  calls              CALL/RET events between this step and the next\n"
        << "  find STR           jump to the first step containing STR\n"
        << "  help, ?            this list\n"
        << "  quit, q            exit\n";
}

inline int run_replay_repl(const ReplayTrace& t) {
    if (t.steps.empty()) {
        std::cerr << "replay: trace contains no STEP events\n";
        return 1;
    }
    std::cout << "loaded " << t.steps.size() << " steps, "
              << t.calls.size() << " call events. Type 'help' for commands.\n";
    int cur = 0;
    while (true) {
        const ReplayStep& s = t.steps[cur];
        std::cout << "(replay) [step " << s.index
                  << " line=" << s.line << ":" << s.col << "]> ";
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }
        auto tokens = split_cmd(line);
        if (tokens.empty()) continue;
        const std::string& cmd = tokens[0];

        if (cmd == "quit" || cmd == "q" || cmd == "exit") break;
        if (cmd == "help" || cmd == "?") { print_help(); continue; }

        if (cmd == "next" || cmd == "n") {
            if (cur + 1 < (int)t.steps.size()) ++cur;
            else std::cout << "(at last step)\n";
            continue;
        }
        if (cmd == "prev" || cmd == "p") {
            if (cur > 0) --cur;
            else std::cout << "(at first step)\n";
            continue;
        }
        if (cmd == "goto") {
            if (tokens.size() != 2) { std::cout << "usage: goto N\n"; continue; }
            int n;
            try { n = std::stoi(tokens[1]); }
            catch (...) { std::cout << "not a number: " << tokens[1] << "\n"; continue; }
            // Find by .index (not array position) since steps may not be 0..N-1
            // densely if the format ever changes. v1 they are dense, but cheap
            // to be future-proof.
            int found = -1;
            for (int i = 0; i < (int)t.steps.size(); ++i)
                if (t.steps[i].index == n) { found = i; break; }
            if (found < 0) std::cout << "no step " << n << " in trace\n";
            else cur = found;
            continue;
        }
        if (cmd == "list" || cmd == "l") {
            if (s.vars.empty()) std::cout << "  (no variables defined yet)\n";
            for (const auto& [name, val] : s.vars) {
                std::cout << "  " << name << " = " << val << "\n";
            }
            continue;
        }
        if (cmd == "inspect" || cmd == "i") {
            if (tokens.size() != 2) { std::cout << "usage: inspect VAR\n"; continue; }
            std::string val = lookup_var(s, tokens[1]);
            if (val.empty()) std::cout << tokens[1] << ": not defined at this step\n";
            else std::cout << tokens[1] << " = " << val << "\n";
            continue;
        }
        if (cmd == "history" || cmd == "h") {
            if (tokens.size() != 2) { std::cout << "usage: history VAR\n"; continue; }
            const std::string& name = tokens[1];
            std::string prev;
            bool any = false;
            for (const auto& step : t.steps) {
                std::string val = lookup_var(step, name);
                if (val.empty()) continue;
                if (val == prev) continue; // only show changes
                std::cout << "  step " << step.index
                          << " line=" << step.line << ":" << step.col
                          << ":  " << name << " = " << val << "\n";
                prev = val;
                any = true;
            }
            if (!any) std::cout << name << ": never defined in this trace\n";
            continue;
        }
        if (cmd == "calls") {
            int next_step = cur + 1 < (int)t.steps.size() ? t.steps[cur + 1].index : -1;
            bool any = false;
            for (const auto& c : t.calls) {
                if (c.after_step != cur) continue; // only events tied to current
                any = true;
                if (c.is_return) {
                    std::cout << "  RET  " << c.fn << " -> " << c.args_or_value << "\n";
                } else {
                    std::cout << "  CALL " << c.fn << "(" << c.args_or_value << ")"
                              << " at line=" << c.line << ":" << c.col << "\n";
                }
            }
            if (!any) {
                std::cout << "  (no CALL/RET events between step " << s.index
                          << " and step " << next_step << ")\n";
            }
            continue;
        }
        if (cmd == "find") {
            if (tokens.size() < 2) { std::cout << "usage: find STR\n"; continue; }
            // Join remaining tokens with spaces in case the user typed a value
            // like "find 99.5" or "find [1, 2, 3]".
            std::string needle = tokens[1];
            for (size_t k = 2; k < tokens.size(); ++k) needle += " " + tokens[k];
            int found = -1;
            for (int i = cur; i < (int)t.steps.size(); ++i) {
                for (const auto& [n, v] : t.steps[i].vars) {
                    if (v.find(needle) != std::string::npos
                     || n.find(needle) != std::string::npos) {
                        found = i; break;
                    }
                }
                if (found >= 0) break;
            }
            if (found < 0) std::cout << "no step containing '" << needle << "' from here on\n";
            else { cur = found; std::cout << "jumped to step " << t.steps[cur].index << "\n"; }
            continue;
        }

        std::cout << "unknown command: " << cmd << " (type 'help')\n";
    }
    return 0;
}

} // namespace knot
