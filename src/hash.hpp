#pragma once
// Content-hashing for AST nodes. We hash a canonical textual serialization
// of each Stmt so that the same source -- regardless of whitespace, line
// numbers, or comments -- produces the same hash. This is the backbone of
// the external pseudocode-annotation system: annotations are stored in a
// sidecar `.annot` file keyed by these hashes, so they survive code edits
// that don't change the structure.
//
// The hash is 64-bit FNV-1a, which is more than enough for "a few hundred
// statements in one file"; collisions at that scale are astronomically rare
// and would only cause one annotation to apply to the wrong line, never a
// crash or correctness issue.

#include "ast.hpp"
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace knot {

inline uint64_t fnv1a_64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

inline std::string hash_hex(uint64_t h) {
    std::ostringstream o;
    o << std::hex << std::setw(16) << std::setfill('0') << h;
    return o.str();
}

// Canonical serialization of an expression. Stable across whitespace and
// span/line/column changes; sensitive to identifiers, literals, and operators.
inline void serialize_expr(const Expr& e, std::ostringstream& o) {
    switch (e.kind) {
        case ExprKind::NumberLit: o << "n(" << e.num << ")"; break;
        case ExprKind::StringLit: o << "s(" << e.str.size() << ":" << e.str << ")"; break;
        case ExprKind::BoolLit:   o << "b(" << (e.boolean ? "T" : "F") << ")"; break;
        case ExprKind::NilLit:    o << "nil"; break;
        case ExprKind::Ident:     o << "i(" << e.str << ")"; break;
        case ExprKind::Unary:
            o << "u(" << (int)e.unop << ",";
            serialize_expr(*e.rhs, o);
            o << ")";
            break;
        case ExprKind::Binary:
            o << "B(" << (int)e.binop << ",";
            serialize_expr(*e.lhs, o);
            o << ",";
            serialize_expr(*e.rhs, o);
            o << ")";
            break;
        case ExprKind::Index:
            o << "X(";
            serialize_expr(*e.callee, o);
            for (const auto& el : e.elems) {
                o << ",";
                serialize_expr(*el, o);
            }
            o << ")";
            break;
        case ExprKind::Slice:
            o << "S(";
            if (e.slice_lo) serialize_expr(*e.slice_lo, o);
            o << ":";
            if (e.slice_hi) serialize_expr(*e.slice_hi, o);
            o << ")";
            break;
        case ExprKind::Call:
            o << "C(";
            serialize_expr(*e.callee, o);
            for (const auto& a : e.elems) {
                o << ",";
                serialize_expr(*a, o);
            }
            o << ")";
            break;
        case ExprKind::VecLit:
            o << "V(";
            for (size_t i = 0; i < e.elems.size(); ++i) {
                if (i) o << ",";
                serialize_expr(*e.elems[i], o);
            }
            o << ")";
            break;
        case ExprKind::MatLit:
            o << "M(";
            for (size_t r = 0; r < e.rows.size(); ++r) {
                if (r) o << ";";
                for (size_t c = 0; c < e.rows[r].size(); ++c) {
                    if (c) o << ",";
                    serialize_expr(*e.rows[r][c], o);
                }
            }
            o << ")";
            break;
    }
}

inline void serialize_stmt(const Stmt& s, std::ostringstream& o);

inline void serialize_block(const std::vector<StmtPtr>& body, std::ostringstream& o) {
    o << "{";
    for (size_t i = 0; i < body.size(); ++i) {
        if (i) o << ";";
        serialize_stmt(*body[i], o);
    }
    o << "}";
}

inline void serialize_stmt(const Stmt& s, std::ostringstream& o) {
    switch (s.kind) {
        case StmtKind::Assign:
            o << "A(";
            if (s.target) serialize_expr(*s.target, o);
            else o << "i(" << s.name << ")";
            o << ",";
            serialize_expr(*s.expr, o);
            o << ")";
            break;
        case StmtKind::CompAssign:
            o << "K(" << (int)s.comp_op << "," << s.name << ",";
            serialize_expr(*s.expr, o);
            o << ")";
            break;
        case StmtKind::ExprStmt:
            o << "E(";
            serialize_expr(*s.expr, o);
            o << ")";
            break;
        case StmtKind::Print:
            o << "P(";
            serialize_expr(*s.expr, o);
            o << ")";
            break;
        case StmtKind::If:
            o << "If(";
            serialize_expr(*s.expr, o);
            o << ",";
            serialize_block(s.body, o);
            o << ",";
            serialize_block(s.else_body, o);
            o << ")";
            break;
        case StmtKind::While:
            o << "W(";
            serialize_expr(*s.expr, o);
            o << ",";
            serialize_block(s.body, o);
            o << ")";
            break;
        case StmtKind::Loop:
            o << "L(" << s.name << ",";
            serialize_expr(*s.expr, o);
            o << ",";
            serialize_block(s.body, o);
            o << ")";
            break;
        case StmtKind::For: {
            const char* tag = "FoTo";
            if (s.for_form == ForForm::InElem) tag = "FoIn";
            if (s.for_form == ForForm::InBoth) tag = "FoBoth";
            o << tag << "(" << s.name;
            if (s.for_form == ForForm::InBoth) o << "," << s.elem_name;
            o << ",";
            serialize_expr(*s.expr, o);
            o << ",";
            serialize_block(s.body, o);
            o << ")";
            break;
        }
        case StmtKind::Block:
            o << "Bk";
            serialize_block(s.body, o);
            break;
        case StmtKind::FnDecl:
            o << "F(" << s.name << ",[";
            for (size_t i = 0; i < s.params.size(); ++i) {
                if (i) o << ",";
                o << s.params[i];
                if (i < s.param_defaults.size() && s.param_defaults[i]) {
                    o << "=";
                    serialize_expr(*s.param_defaults[i], o);
                }
            }
            o << "],";
            serialize_block(s.body, o);
            o << ")";
            break;
        case StmtKind::Return:
            o << "R(";
            if (s.expr) serialize_expr(*s.expr, o);
            o << ")";
            break;
        case StmtKind::Break:    o << "Br"; break;
        case StmtKind::Continue: o << "Co"; break;
        case StmtKind::TestDecl:
            o << "T(" << s.name << ",";
            serialize_block(s.body, o);
            o << ")";
            break;
    }
}

inline std::string stmt_hash(const Stmt& s) {
    std::ostringstream o;
    serialize_stmt(s, o);
    return hash_hex(fnv1a_64(o.str()));
}

} // namespace knot
