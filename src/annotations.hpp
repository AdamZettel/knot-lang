#pragma once
// Loads pseudocode annotations from a sidecar .annot file.
//
// File format (LLM-friendly, deliberately boring):
//
//   # comments allowed; lines starting with # are skipped
//   HASH<TAB>DESCRIPTION
//   HASH1+HASH2<TAB>DESCRIPTION
//
// A single HASH (16 hex chars) matches one statement. A composite key
// HASH1+HASH2+...+HASHN matches a run of N consecutive statements at the
// same scope (in source order).
//
// At step-time, the interpreter looks up each statement:
//   1. First try a composite match starting at this stmt: build keys for
//      runs of length k=K_max, K_max-1, ..., 1 (where K_max is some bound)
//      and pick the longest matching one. That match "consumes" k stmts.
//   2. If no composite hits, look up the single-stmt hash.
//   3. If neither, no annotation prints for this stmt.

#include "ast.hpp"
#include "hash.hpp"
#include <algorithm>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace knot {

struct Annotations {
    // hash -> description.  Composite keys are stored as "H1+H2+..." strings.
    std::unordered_map<std::string, std::string> table;

    // Largest k we'll ever look up as a composite. Capped to keep step-mode
    // fast even on long files.
    static constexpr int kMaxComposite = 8;

    bool empty() const { return table.empty(); }
    size_t size() const { return table.size(); }

    // Lookup composite or single. Returns the description and the number of
    // stmts consumed (>= 1) if found; returns {"", 0} if no match.
    struct Hit { std::string desc; int consumed = 0; };
    Hit lookup(const std::vector<std::string>& hashes, size_t start) const {
        int max_k = std::min<int>(kMaxComposite, (int)(hashes.size() - start));
        // Try longest composite first; favor specific matches.
        for (int k = max_k; k >= 2; --k) {
            std::string key = hashes[start];
            for (int i = 1; i < k; ++i) {
                key += "+";
                key += hashes[start + i];
            }
            auto it = table.find(key);
            if (it != table.end()) return {it->second, k};
        }
        auto it = table.find(hashes[start]);
        if (it != table.end()) return {it->second, 1};
        return {};
    }
};

inline Annotations load_annotations(const std::string& path) {
    Annotations a;
    std::ifstream f(path);
    if (!f) return a;
    std::string line;
    while (std::getline(f, line)) {
        // Strip a trailing \r in case the file came from Windows.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        // Split on first tab.
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue; // malformed; ignore quietly
        std::string key = line.substr(0, tab);
        std::string desc = line.substr(tab + 1);
        a.table[key] = desc;
    }
    return a;
}

} // namespace knot
