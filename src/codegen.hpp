#pragma once
// knot → C code generator (v1).
//
// Each expression is emitted as a C expression string and has an inferred
// type. Each statement is emitted as a C statement. We monomorphize: a
// knot variable holding a number compiles to a C `double`, a vec to a
// `knot_vec`, etc. v1 doesn't support polymorphism on reassignment — once a
// name is bound to a type, it must stay that type within a scope. The
// interpreter is permissive about this; the transpiler isn't.
//
// What v1 supports: number/bool/string literals, identifiers, all binops,
// unary, calls to known builtins (print, sqrt, abs, sin, cos, exp, log,
// rows, cols, len, zeros/ones for vec only, dot, norm), function defs
// with double-only signatures, return, if/else, while, loop n / loop n
// as i, assignment, compound assignment, vec literals (all-numeric),
// vec/mat indexing read (single-elt), vec/mat indexing write.
//
// What v1 does NOT support: heterogeneous lists, closures, default args,
// matrix literals, slicing, string concat, tagged-index checks (they're
// erased), the @ matmul operator on matrices (we emit knot_mat_mul but only
// for known-mat operands).
//
// Numerical higher-order functions ARE supported for arities 1-3
// (FnD_D / FnDD_D / FnDDD_D), which covers rk4 / bisect / newton /
// simpson / trapezoid / golden_section. Beyond 3 args, add a new
// CType entry and update emit_call / c_decl / scan_expr_for_type
// in tandem.

#include "ast.hpp"
#include "diag.hpp"
#include "parser.hpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace knot {

enum class CType {
    Num,        // double
    Bool,       // int (0/1)
    Vec,        // knot_vec
    Mat,        // knot_mat
    Str,        // const char* (literals only; no concat in v1)
    Nil,        // void return / placeholder
    FnD_D,      // knot_clos_d_d        — bisect/newton/simpson/trapezoid/golden_section, closure-capable
    FnDD_D,     // knot_clos_dd_d       — rk4 (dydt = f(t, y))
    FnDDD_D,    // knot_clos_ddd_d      — 3-arg numerical callbacks
    Unknown,    // could not infer; will likely cause a downstream error
};

inline const char* ctype_name(CType t) {
    switch (t) {
        case CType::Num:   return "double";
        case CType::Bool:  return "int";
        case CType::Vec:   return "knot_vec";
        case CType::Mat:   return "knot_mat";
        case CType::Str:   return "const char*";
        case CType::Nil:   return "void";
        case CType::FnD_D:   return "knot_clos_d_d";
        case CType::FnDD_D:  return "knot_clos_dd_d";
        case CType::FnDDD_D: return "knot_clos_ddd_d";
        case CType::Unknown: return "/*unknown*/ void*";
    }
    return "/*?*/";
}

// A scope of name → type, chained like the interpreter's Env.
struct CScope {
    std::unordered_map<std::string, CType> types;
    CScope* parent = nullptr;
    CScope() = default;
    explicit CScope(CScope* p) : parent(p) {}
    CType lookup(const std::string& name) const {
        auto it = types.find(name);
        if (it != types.end()) return it->second;
        if (parent) return parent->lookup(name);
        return CType::Unknown;
    }
    bool has(const std::string& name) const {
        if (types.count(name)) return true;
        if (parent) return parent->has(name);
        return false;
    }
    bool has_local(const std::string& name) const {
        return types.count(name) > 0;
    }
    void define(const std::string& name, CType t) { types[name] = t; }
};

class Codegen {
public:
    // Generate a complete C program from `program`. Returns the source as
    // a string ready to write to disk and pass to a C compiler.
    std::string generate(const std::vector<StmtPtr>& program,
                         const std::string& source_filename) {
        out_.str(""); out_.clear();
        prelude_.str(""); prelude_.clear();
        fn_defs_.str(""); fn_defs_.clear();

        source_filename_ = source_filename;

        // Two passes. First, register signatures for every top-level
        // function so they can call each other regardless of source order.
        // Second, emit bodies + top-level stmts.
        CScope global_scope;
        for (const auto& s : program) {
            if (s->kind == StmtKind::FnDecl) register_fn(*s);
        }

        // Now re-run signature inference with knowledge of all other
        // functions' tentative signatures. (One additional pass to refine
        // param types using cross-function hints.)
        for (const auto& s : program) {
            if (s->kind == StmtKind::FnDecl) register_fn(*s);
        }

        // Emit bodies (these write into fn_defs_) and top-level stmts
        // (these write into main_body).
        std::ostringstream main_body;
        for (const auto& s : program) {
            if (s->kind == StmtKind::FnDecl) {
                // Try to emit. If it fails (e.g. references unsupported
                // features), skip this function -- user code that calls it
                // will fail at C-compile time, but everything else compiles.
                std::ostringstream backup_fn_defs;
                backup_fn_defs.str(fn_defs_.str());
                try {
                    emit_fn(*s, &global_scope);
                } catch (const Diag& d) {
                    // Roll back fn_defs_ to before this function and emit
                    // a stub that aborts at runtime.
                    fn_defs_.str(backup_fn_defs.str());
                    fn_defs_.clear();
                    fn_defs_.seekp(0, std::ios::end);
                    auto it = fn_sigs_.find(s->name);
                    if (it != fn_sigs_.end()) {
                        const FnSig& sig = it->second;
                        fn_defs_ << "/* skipped: " << s->name
                                 << " (codegen failed: " << d.what() << ") */\n";
                        fn_defs_ << ctype_name(sig.ret_type) << " " << s->name << "(";
                        for (size_t i = 0; i < sig.param_types.size(); ++i) {
                            if (i) fn_defs_ << ", ";
                            fn_defs_ << c_decl(sig.param_types[i], "_" + std::to_string(i));
                        }
                        fn_defs_ << ") {\n";
                        for (size_t i = 0; i < sig.param_types.size(); ++i)
                            fn_defs_ << "    (void)_" << i << ";\n";
                        fn_defs_ << "    fprintf(stderr, \"" << s->name
                                 << ": not supported in --exec mode\\n\");\n";
                        fn_defs_ << "    exit(1);\n";
                        fn_defs_ << "}\n\n";
                    }
                }
            } else {
                emit_stmt(*s, &global_scope, main_body, 1);
            }
        }

        // Emit forward declarations so functions can refer to each other
        // regardless of source order. (The bodies in fn_defs_ are still
        // emitted in source order.)
        std::ostringstream forward_decls;
        for (const auto& s : program) {
            if (s->kind != StmtKind::FnDecl) continue;
            auto it = fn_sigs_.find(s->name);
            if (it == fn_sigs_.end()) continue;
            const FnSig& sig = it->second;
            forward_decls << ctype_name(sig.ret_type) << " " << s->name << "(";
            for (size_t i = 0; i < sig.param_types.size(); ++i) {
                if (i) forward_decls << ", ";
                forward_decls << ctype_name(sig.param_types[i]);
            }
            forward_decls << ");\n";
        }

        std::ostringstream result;
        result << "// auto-generated by knot --cc from "
               << source_filename << "\n";
        result << "#include \"runtime.h\"\n\n";
        result << forward_decls.str() << "\n";
        result << prelude_.str();
        result << fn_defs_.str();
        result << "int main(void) {\n";
        result << main_body.str();
        result << "    return 0;\n";
        result << "}\n";
        return result.str();
    }

private:
    std::ostringstream out_;
    std::ostringstream prelude_;
    std::ostringstream fn_defs_;
    std::string source_filename_;

    // Track type signatures of user-defined functions so calls can pick
    // the right C arg/return types.
    struct FnSig {
        std::vector<CType> param_types;
        CType ret_type = CType::Num;
        // Default-value expressions (one per param; null means required).
        // We hold raw pointers into the AST, which lives at least as long
        // as codegen.
        std::vector<const Expr*> param_defaults;
    };
    std::unordered_map<std::string, FnSig> fn_sigs_;

    // Track (def_name, arity) pairs we've already emitted a closure thunk
    // for; pass-by-value of a bare `def` to a higher-order function wraps
    // it in a (fn-pointer, NULL env) closure literal that calls through
    // one of these thunks.
    std::set<std::pair<std::string, int>> emitted_thunks_;

    // Anonymous FnExpr counter (used for `fn(x) -> EXPR` lifting).
    int next_fnexpr_id_ = 0;

    // ---------------- Expression emission ----------------

    struct ExprResult {
        std::string code;
        CType type;
    };

    [[noreturn]] static void fail(Span s, const std::string& msg) {
        throw Diag(s, "codegen: " + msg);
    }

    ExprResult emit_expr(const Expr& e, CScope* scope) {
        switch (e.kind) {
            case ExprKind::NumberLit: {
                std::ostringstream o;
                // Always emit as a double literal; use enough precision.
                o.precision(17);
                o << e.num;
                std::string s = o.str();
                // Ensure it's recognized as a floating literal.
                if (s.find('.') == std::string::npos
                 && s.find('e') == std::string::npos
                 && s.find('E') == std::string::npos
                 && s.find("inf") == std::string::npos
                 && s.find("nan") == std::string::npos) {
                    s += ".0";
                }
                return {s, CType::Num};
            }
            case ExprKind::BoolLit:
                return {e.boolean ? "1" : "0", CType::Bool};
            case ExprKind::NilLit: {
                // In compiled mode there's no "nil" sentinel. We map None to
                // NaN when the surrounding context wants a num -- which is
                // the only context where None ever appears in numerical
                // stdlib code ("return None" from a failed root finder).
                return {"(0.0/0.0)", CType::Num};
            }
            case ExprKind::StringLit: {
                // Emit a C string literal with basic escaping.
                std::ostringstream o;
                o << '"';
                for (char c : e.str) {
                    switch (c) {
                        case '"':  o << "\\\""; break;
                        case '\\': o << "\\\\"; break;
                        case '\n': o << "\\n"; break;
                        case '\t': o << "\\t"; break;
                        default:
                            if ((unsigned char)c < 32) {
                                char buf[8];
                                std::snprintf(buf, sizeof buf, "\\x%02x", (unsigned char)c);
                                o << buf;
                            } else {
                                o << c;
                            }
                    }
                }
                o << '"';
                return {o.str(), CType::Str};
            }
            case ExprKind::Ident: {
                CType t = scope->lookup(e.str);
                if (t == CType::Unknown) {
                    // Could be a user-defined function used as a first-class
                    // value (passing it to bisect, newton, etc.).  We wrap
                    // the bare `def` in an env-ignoring thunk and return a
                    // closure literal (struct of {fn_ptr, NULL env}).
                    auto it = fn_sigs_.find(e.str);
                    if (it != fn_sigs_.end()) {
                        const FnSig& sig = it->second;
                        bool all_num = sig.ret_type == CType::Num;
                        for (CType p : sig.param_types)
                            if (p != CType::Num) { all_num = false; break; }
                        int arity = (int)sig.param_types.size();
                        if (all_num && arity >= 1 && arity <= 3) {
                            std::string thunk = emit_def_thunk(e.str, arity);
                            CType ct = arity == 1 ? CType::FnD_D
                                     : arity == 2 ? CType::FnDD_D
                                     :              CType::FnDDD_D;
                            std::string clos_t = ctype_name(ct);
                            std::string code = "((" + clos_t + "){"
                                + thunk + ", NULL})";
                            return {code, ct};
                        }
                        fail(e.span, "function " + e.str
                            + " has signature that isn't passable in --cc v1"
                            + " (need 1-3 Num args, Num return)");
                    }
                    fail(e.span, "unknown name: " + e.str);
                }
                return {e.str, t};
            }
            case ExprKind::Unary: {
                ExprResult r = emit_expr(*e.rhs, scope);
                if (e.unop == UnOp::Neg) {
                    if (r.type != CType::Num)
                        fail(e.span, "unary - needs num, got "
                                     + std::string(ctype_name(r.type)));
                    return {"(-(" + r.code + "))", CType::Num};
                } else { // Not
                    if (r.type != CType::Bool && r.type != CType::Num)
                        fail(e.span, "unary ! needs bool/num");
                    return {"(!(" + r.code + "))", CType::Bool};
                }
            }
            case ExprKind::Binary: {
                return emit_binary(e, scope);
            }
            case ExprKind::Call: {
                return emit_call(e, scope);
            }
            case ExprKind::Index: {
                return emit_index(e, scope);
            }
            case ExprKind::VecLit: {
                // All-numeric only in v1.
                std::vector<std::string> elt_codes;
                elt_codes.reserve(e.elems.size());
                for (const auto& el : e.elems) {
                    ExprResult r = emit_expr(*el, scope);
                    if (r.type != CType::Num)
                        fail(el->span, "vec literal element must be num");
                    elt_codes.push_back(r.code);
                }
                // Emit a compound literal we pass to knot_vec_from.
                std::ostringstream o;
                o << "knot_vec_from(" << elt_codes.size()
                  << ", (double[]){";
                for (size_t i = 0; i < elt_codes.size(); ++i) {
                    if (i) o << ", ";
                    o << elt_codes[i];
                }
                o << "})";
                return {o.str(), CType::Vec};
            }
            case ExprKind::MatLit: {
                // Build via a GCC statement-expression: allocate, set every
                // element, return the matrix.
                if (e.rows.empty())
                    fail(e.span, "empty matrix literal");
                size_t nrows = e.rows.size();
                size_t ncols = e.rows[0].size();
                for (const auto& row : e.rows) {
                    if (row.size() != ncols)
                        fail(e.span, "matrix rows must have equal length");
                }
                std::ostringstream o;
                o << "({ knot_mat _m = knot_mat_new(" << nrows << ", " << ncols << "); ";
                for (size_t i = 0; i < nrows; ++i) {
                    for (size_t j = 0; j < ncols; ++j) {
                        ExprResult cell = emit_expr(*e.rows[i][j], scope);
                        if (cell.type != CType::Num)
                            fail(e.rows[i][j]->span,
                                 "matrix element must be num");
                        o << "knot_mat_set(_m, " << i << ", " << j << ", "
                          << cell.code << "); ";
                    }
                }
                o << "_m; })";
                return {o.str(), CType::Mat};
            }
            case ExprKind::Slice:
                fail(e.span, "slices not supported in --cc v1");
            case ExprKind::FnExpr:
                return emit_fnexpr(e, scope);
        }
        fail(e.span, "internal: unhandled expr kind");
    }

    ExprResult emit_binary(const Expr& e, CScope* scope) {
        ExprResult l = emit_expr(*e.lhs, scope);
        ExprResult r = emit_expr(*e.rhs, scope);

        auto cmp_op = [&](const char* op) -> ExprResult {
            if (l.type != CType::Num || r.type != CType::Num)
                fail(e.span, std::string("comparison ") + op + " needs num");
            return {"(" + l.code + " " + op + " " + r.code + ")", CType::Bool};
        };
        auto logical = [&](const char* op) -> ExprResult {
            // Accept num or bool on either side; treat 0/nonzero like C.
            return {"(" + l.code + " " + op + " " + r.code + ")", CType::Bool};
        };

        switch (e.binop) {
            case BinOp::Add:
                if (l.type == CType::Num && r.type == CType::Num)
                    return {"(" + l.code + " + " + r.code + ")", CType::Num};
                if (l.type == CType::Vec && r.type == CType::Vec)
                    return {"knot_vec_add(" + l.code + ", " + r.code + ")", CType::Vec};
                fail(e.span, std::string("+ undefined for ")
                             + ctype_name(l.type) + " and " + ctype_name(r.type));
            case BinOp::Sub:
                if (l.type == CType::Num && r.type == CType::Num)
                    return {"(" + l.code + " - " + r.code + ")", CType::Num};
                if (l.type == CType::Vec && r.type == CType::Vec)
                    return {"knot_vec_sub(" + l.code + ", " + r.code + ")", CType::Vec};
                fail(e.span, "- undefined for these types");
            case BinOp::Mul:
                if (l.type == CType::Num && r.type == CType::Num)
                    return {"(" + l.code + " * " + r.code + ")", CType::Num};
                if (l.type == CType::Num && r.type == CType::Vec)
                    return {"knot_vec_scale(" + r.code + ", " + l.code + ")", CType::Vec};
                if (l.type == CType::Vec && r.type == CType::Num)
                    return {"knot_vec_scale(" + l.code + ", " + r.code + ")", CType::Vec};
                fail(e.span, "* undefined for these types");
            case BinOp::Div:
                if (l.type == CType::Num && r.type == CType::Num)
                    return {"(" + l.code + " / " + r.code + ")", CType::Num};
                if (l.type == CType::Vec && r.type == CType::Num)
                    return {"knot_vec_scale(" + l.code + ", 1.0 / (" + r.code + "))", CType::Vec};
                fail(e.span, "/ undefined for these types");
            case BinOp::Mod:
                if (l.type == CType::Num && r.type == CType::Num)
                    return {"fmod(" + l.code + ", " + r.code + ")", CType::Num};
                fail(e.span, "% needs num operands");
            case BinOp::Matmul:
                if (l.type == CType::Mat && r.type == CType::Mat)
                    return {"knot_mat_mul(" + l.code + ", " + r.code + ")", CType::Mat};
                if (l.type == CType::Mat && r.type == CType::Vec)
                    return {"knot_mat_vec(" + l.code + ", " + r.code + ")", CType::Vec};
                if (l.type == CType::Vec && r.type == CType::Vec)
                    return {"knot_vec_dot(" + l.code + ", " + r.code + ")", CType::Num};
                fail(e.span, "@ undefined for these types");
            case BinOp::Eq:     return cmp_op("==");
            case BinOp::Ne:     return cmp_op("!=");
            case BinOp::Lt:     return cmp_op("<");
            case BinOp::Le:     return cmp_op("<=");
            case BinOp::Gt:     return cmp_op(">");
            case BinOp::Ge:     return cmp_op(">=");
            case BinOp::And:    return logical("&&");
            case BinOp::Or:     return logical("||");
        }
        fail(e.span, "internal: unhandled binop");
    }

    ExprResult emit_call(const Expr& e, CScope* scope) {
        if (e.callee->kind != ExprKind::Ident)
            fail(e.callee->span, "callee must be a name in --cc v1");
        const std::string& name = e.callee->str;

        // If the callee is a *variable* of closure type (e.g. a parameter
        // `f` declared FnD_D / FnDD_D / FnDDD_D), unpack the fat pointer
        // and call: f.fn(f.env, arg0, arg1, ...).
        CType var_t = scope->lookup(name);
        if (var_t == CType::FnD_D || var_t == CType::FnDD_D || var_t == CType::FnDDD_D) {
            size_t expected = (var_t == CType::FnD_D)  ? 1
                            : (var_t == CType::FnDD_D) ? 2
                            : 3;
            if (e.elems.size() != expected)
                fail(e.span, "closure call needs "
                    + std::to_string(expected) + " args, got "
                    + std::to_string(e.elems.size()));
            std::string call = name + ".fn(" + name + ".env";
            for (size_t i = 0; i < expected; ++i) {
                ExprResult a = emit_expr(*e.elems[i], scope);
                if (a.type != CType::Num)
                    fail(e.elems[i]->span, "closure arg must be num");
                call += ", ";
                call += a.code;
            }
            call += ")";
            return {"(" + call + ")", CType::Num};
        }

        // Evaluate args.
        std::vector<ExprResult> args;
        args.reserve(e.elems.size());
        for (const auto& a : e.elems) args.push_back(emit_expr(*a, scope));

        // Builtins first.
        auto join = [&](const std::string& fn) {
            std::ostringstream o;
            o << fn << "(";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) o << ", ";
                o << args[i].code;
            }
            o << ")";
            return o.str();
        };

        if (name == "print") {
            // Multi-arg print: emit a sequence of prints separated by spaces,
            // then a newline. Returns nil (we emit a comma expression
            // evaluating to 0.0; print is always a statement in practice).
            std::ostringstream o;
            o << "(";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) o << ", putchar(' '), ";
                switch (args[i].type) {
                    case CType::Num:  o << "knot_print_num("  << args[i].code << ")"; break;
                    case CType::Bool: o << "fputs(("        << args[i].code << ")?\"true\":\"false\", stdout)"; break;
                    case CType::Str:  o << "knot_print_str("  << args[i].code << ")"; break;
                    case CType::Vec:  o << "knot_print_vec("  << args[i].code << ")"; break;
                    case CType::Mat:  o << "knot_print_mat("  << args[i].code << ")"; break;
                    default: fail(e.elems[i]->span, "print: unsupported arg type");
                }
            }
            if (args.empty()) o << "(void)0";
            o << ", knot_print_newline(), 0.0)";
            return {o.str(), CType::Num};  // pretend it's num; result unused
        }
        if (name == "sqrt") { if (args.size() != 1 || args[0].type != CType::Num) fail(e.span, "sqrt(num)"); return {join("knot_sqrt"), CType::Num}; }
        if (name == "abs")  { if (args.size() != 1 || args[0].type != CType::Num) fail(e.span, "abs(num)");  return {join("knot_abs"),  CType::Num}; }
        if (name == "sin")  { if (args.size() != 1 || args[0].type != CType::Num) fail(e.span, "sin(num)");  return {join("knot_sin"),  CType::Num}; }
        if (name == "cos")  { if (args.size() != 1 || args[0].type != CType::Num) fail(e.span, "cos(num)");  return {join("knot_cos"),  CType::Num}; }
        if (name == "exp")  { if (args.size() != 1 || args[0].type != CType::Num) fail(e.span, "exp(num)");  return {join("knot_exp"),  CType::Num}; }
        if (name == "log")  { if (args.size() != 1 || args[0].type != CType::Num) fail(e.span, "log(num)");  return {join("knot_log"),  CType::Num}; }

        // Extensions backed by the C++ runtime (runtime_ext.cpp).
        if (name == "sort_vec") {
            if (args.size() != 1 || args[0].type != CType::Vec)
                fail(e.span, "sort_vec(vec)");
            return {"(knot_sort_vec(" + args[0].code + "), 0.0)", CType::Num};
        }
        if (name == "rng_seed") {
            if (args.size() != 1 || args[0].type != CType::Num)
                fail(e.span, "rng_seed(num)");
            return {"(knot_rng_seed(" + args[0].code + "), 0.0)", CType::Num};
        }
        if (name == "rng_uniform") {
            if (!args.empty()) fail(e.span, "rng_uniform() takes no args");
            return {"knot_rng_uniform()", CType::Num};
        }
        if (name == "rng_normal") {
            if (!args.empty()) fail(e.span, "rng_normal() takes no args");
            return {"knot_rng_normal()", CType::Num};
        }
        if (name == "read_csv") {
            if (args.size() != 1 || args[0].type != CType::Str)
                fail(e.span, "read_csv(path)");
            return {"knot_read_csv(" + args[0].code + ")", CType::Mat};
        }
        if (name == "write_csv") {
            if (args.size() != 2 || args[0].type != CType::Mat
             || args[1].type != CType::Str)
                fail(e.span, "write_csv(mat, path)");
            return {"((double)knot_write_csv(" + args[0].code + ", "
                    + args[1].code + "))", CType::Num};
        }
        if (name == "rows") { if (args.size() != 1 || args[0].type != CType::Mat) fail(e.span, "rows(mat)"); return {"((double)knot_mat_rows(" + args[0].code + "))", CType::Num}; }
        if (name == "cols") { if (args.size() != 1 || args[0].type != CType::Mat) fail(e.span, "cols(mat)"); return {"((double)knot_mat_cols(" + args[0].code + "))", CType::Num}; }
        if (name == "len") {
            if (args.size() != 1) fail(e.span, "len(vec)");
            if (args[0].type == CType::Vec) return {"((double)knot_vec_len(" + args[0].code + "))", CType::Num};
            fail(e.span, "len: only vec in --cc v1");
        }
        if (name == "zeros") {
            if (args.size() == 1 && args[0].type == CType::Num)
                return {"knot_vec_new((int)(" + args[0].code + "))", CType::Vec};
            if (args.size() == 2 && args[0].type == CType::Num && args[1].type == CType::Num)
                return {"knot_mat_new((int)(" + args[0].code + "), (int)(" + args[1].code + "))", CType::Mat};
            fail(e.span, "zeros: bad args");
        }
        if (name == "ones") {
            // Build via fill loop. We inline a helper-style expression:
            // a ({ ... }) statement-expression block (GCC extension).
            if (args.size() == 1 && args[0].type == CType::Num) {
                std::string code = "({ knot_vec _r = knot_vec_new((int)(" + args[0].code
                    + ")); for (int _i = 0; _i < _r.n; ++_i) _r.data[_i] = 1.0; _r; })";
                return {code, CType::Vec};
            }
            fail(e.span, "ones(n) only in --cc v1");
        }
        if (name == "dot") {
            if (args.size() == 2 && args[0].type == CType::Vec && args[1].type == CType::Vec)
                return {"knot_vec_dot(" + args[0].code + ", " + args[1].code + ")", CType::Num};
            fail(e.span, "dot(vec, vec)");
        }
        if (name == "norm") {
            if (args.size() == 1 && args[0].type == CType::Vec)
                return {"knot_vec_norm(" + args[0].code + ")", CType::Num};
            fail(e.span, "norm(vec)");
        }
        if (name == "at") {
            // Untagged indexing — same as knot_vec_get / knot_mat_get.
            if (args.size() == 2 && args[0].type == CType::Vec && args[1].type == CType::Num)
                return {"knot_vec_get(" + args[0].code + ", (int)(" + args[1].code + "))", CType::Num};
            if (args.size() == 3 && args[0].type == CType::Mat
             && args[1].type == CType::Num && args[2].type == CType::Num)
                return {"knot_mat_get(" + args[0].code + ", (int)(" + args[1].code
                        + "), (int)(" + args[2].code + "))", CType::Num};
            fail(e.span, "at: bad args");
        }
        if (name == "set") {
            // set is a statement-like call; we emit as a comma expr returning 0.0
            if (args.size() == 3 && args[0].type == CType::Vec
             && args[1].type == CType::Num && args[2].type == CType::Num)
                return {"(knot_vec_set(" + args[0].code + ", (int)(" + args[1].code
                        + "), " + args[2].code + "), 0.0)", CType::Num};
            if (args.size() == 4 && args[0].type == CType::Mat
             && args[1].type == CType::Num && args[2].type == CType::Num
             && args[3].type == CType::Num)
                return {"(knot_mat_set(" + args[0].code + ", (int)(" + args[1].code
                        + "), (int)(" + args[2].code + "), " + args[3].code + "), 0.0)", CType::Num};
            fail(e.span, "set: bad args");
        }

        // User-defined function call.
        auto it = fn_sigs_.find(name);
        if (it == fn_sigs_.end())
            fail(e.callee->span, "unknown function: " + name);
        const FnSig& sig = it->second;
        if (args.size() > sig.param_types.size())
            fail(e.span, "function " + name + " takes at most "
                         + std::to_string(sig.param_types.size())
                         + " args, got " + std::to_string(args.size()));
        // Fill in defaults for any missing trailing args.
        for (size_t i = args.size(); i < sig.param_types.size(); ++i) {
            if (!sig.param_defaults[i]) {
                fail(e.span, "function " + name + " arg "
                             + std::to_string(i) + " is required");
            }
            ExprResult d = emit_expr(*sig.param_defaults[i], scope);
            args.push_back(d);
        }
        for (size_t i = 0; i < args.size(); ++i) {
            if (sig.param_types[i] != args[i].type
             && sig.param_types[i] != CType::Unknown) {
                // Allow num→num literal coercions silently; otherwise complain.
                fail(e.elems.size() > i ? e.elems[i]->span : e.span,
                     "arg " + std::to_string(i) + ": expected "
                     + ctype_name(sig.param_types[i]) + ", got "
                     + ctype_name(args[i].type));
            }
        }
        return {join(name), sig.ret_type};
    }

    ExprResult emit_index(const Expr& e, CScope* scope) {
        ExprResult c = emit_expr(*e.callee, scope);
        if (c.type == CType::Vec) {
            if (e.elems.size() != 1) fail(e.span, "vec index needs 1 int");
            ExprResult i = emit_expr(*e.elems[0], scope);
            return {"knot_vec_get(" + c.code + ", (int)(" + i.code + "))", CType::Num};
        }
        if (c.type == CType::Mat) {
            if (e.elems.size() != 2) fail(e.span, "mat index needs 2 ints");
            ExprResult i = emit_expr(*e.elems[0], scope);
            ExprResult j = emit_expr(*e.elems[1], scope);
            return {"knot_mat_get(" + c.code + ", (int)(" + i.code
                    + "), (int)(" + j.code + "))", CType::Num};
        }
        fail(e.span, std::string("cannot index ") + ctype_name(c.type));
    }

    // ---------------- Statement emission ----------------

    void indent(std::ostream& o, int depth) {
        for (int i = 0; i < depth; ++i) o << "    ";
    }

    void emit_stmt(const Stmt& s, CScope* scope,
                   std::ostream& body, int depth) {
        // (We used to emit #line directives here, but when the stdlib is
        // mixed in with user code, the line numbers point into the stdlib
        // while the filename is the user's -- so C-compiler errors become
        // misleading. Better to omit them in v1.)

        switch (s.kind) {
            case StmtKind::Assign: {
                if (s.target) {
                    // v[i] = expr  or  M[i, j] = expr
                    if (s.target->kind != ExprKind::Index)
                        fail(s.span, "indexed assign target must be Index");
                    ExprResult c = emit_expr(*s.target->callee, scope);
                    ExprResult val = emit_expr(*s.expr, scope);
                    if (val.type != CType::Num)
                        fail(s.span, "indexed assign needs num value");
                    indent(body, depth);
                    if (c.type == CType::Vec) {
                        if (s.target->elems.size() != 1)
                            fail(s.target->span, "vec index needs 1");
                        ExprResult i = emit_expr(*s.target->elems[0], scope);
                        body << "knot_vec_set(" << c.code << ", (int)(" << i.code
                             << "), " << val.code << ");\n";
                    } else if (c.type == CType::Mat) {
                        if (s.target->elems.size() != 2)
                            fail(s.target->span, "mat index needs 2");
                        ExprResult ix = emit_expr(*s.target->elems[0], scope);
                        ExprResult jx = emit_expr(*s.target->elems[1], scope);
                        body << "knot_mat_set(" << c.code << ", (int)("
                             << ix.code << "), (int)(" << jx.code
                             << "), " << val.code << ");\n";
                    } else {
                        fail(s.span, "indexed assign: bad container type");
                    }
                    return;
                }
                ExprResult r = emit_expr(*s.expr, scope);
                indent(body, depth);
                if (scope->has(s.name)) {
                    CType existing = scope->lookup(s.name);
                    if (existing != r.type)
                        fail(s.span, "cannot reassign '" + s.name + "' to a different type");
                    body << s.name << " = " << r.code << ";\n";
                } else {
                    scope->define(s.name, r.type);
                    body << ctype_name(r.type) << " " << s.name
                         << " = " << r.code << ";\n";
                }
                return;
            }
            case StmtKind::CompAssign: {
                if (!scope->has(s.name))
                    fail(s.span, "compound assign on undefined name: " + s.name);
                ExprResult r = emit_expr(*s.expr, scope);
                if (r.type != CType::Num)
                    fail(s.span, "compound assign needs num RHS");
                const char* op = "+=";
                switch (s.comp_op) {
                    case BinOp::Add: op = "+="; break;
                    case BinOp::Sub: op = "-="; break;
                    case BinOp::Mul: op = "*="; break;
                    case BinOp::Div: op = "/="; break;
                    default: fail(s.span, "unsupported compound op");
                }
                indent(body, depth);
                body << s.name << " " << op << " " << r.code << ";\n";
                return;
            }
            case StmtKind::ExprStmt: {
                ExprResult r = emit_expr(*s.expr, scope);
                indent(body, depth);
                body << "(void)(" << r.code << ");\n";
                return;
            }
            case StmtKind::Print: {
                // Legacy path; new code uses print() function call.
                if (s.expr) {
                    ExprResult r = emit_expr(*s.expr, scope);
                    indent(body, depth);
                    switch (r.type) {
                        case CType::Num:  body << "knot_print_num("  << r.code << ");"; break;
                        case CType::Bool: body << "fputs((" << r.code << ")?\"true\":\"false\", stdout);"; break;
                        case CType::Str:  body << "knot_print_str("  << r.code << ");"; break;
                        case CType::Vec:  body << "knot_print_vec("  << r.code << ");"; break;
                        case CType::Mat:  body << "knot_print_mat("  << r.code << ");"; break;
                        default: fail(s.span, "print: unsupported type");
                    }
                    body << " knot_print_newline();\n";
                } else {
                    indent(body, depth);
                    body << "knot_print_newline();\n";
                }
                return;
            }
            case StmtKind::Return: {
                if (s.expr) {
                    ExprResult r = emit_expr(*s.expr, scope);
                    indent(body, depth);
                    body << "return " << r.code << ";\n";
                } else {
                    indent(body, depth);
                    body << "return;\n";
                }
                return;
            }
            case StmtKind::Break:
                indent(body, depth);
                body << "break;\n";
                return;
            case StmtKind::Continue:
                indent(body, depth);
                body << "continue;\n";
                return;
            case StmtKind::If: {
                ExprResult cond = emit_expr(*s.expr, scope);
                indent(body, depth);
                body << "if (" << cond.code << ") {\n";
                {
                    CScope sub(scope);
                    for (const auto& st : s.body) emit_stmt(*st, &sub, body, depth + 1);
                }
                indent(body, depth);
                body << "}";
                if (!s.else_body.empty()) {
                    body << " else {\n";
                    CScope sub(scope);
                    for (const auto& st : s.else_body) emit_stmt(*st, &sub, body, depth + 1);
                    indent(body, depth);
                    body << "}";
                }
                body << "\n";
                return;
            }
            case StmtKind::While: {
                indent(body, depth);
                body << "while (1) {\n";
                ExprResult cond = emit_expr(*s.expr, scope);
                indent(body, depth + 1);
                body << "if (!(" << cond.code << ")) break;\n";
                {
                    CScope sub(scope);
                    for (const auto& st : s.body) emit_stmt(*st, &sub, body, depth + 1);
                }
                indent(body, depth);
                body << "}\n";
                return;
            }
            case StmtKind::Loop: {
                ExprResult over = emit_expr(*s.expr, scope);
                if (over.type == CType::Num) {
                    indent(body, depth);
                    body << "{\n";
                    indent(body, depth + 1);
                    body << "int _loop_n = (int)(" << over.code << ");\n";
                    indent(body, depth + 1);
                    body << "for (int " << s.name << " = 0; " << s.name
                         << " < _loop_n; ++" << s.name << ") {\n";
                    {
                        CScope sub(scope);
                        sub.define(s.name, CType::Num);
                        for (const auto& st : s.body) emit_stmt(*st, &sub, body, depth + 2);
                    }
                    indent(body, depth + 1);
                    body << "}\n";
                    indent(body, depth);
                    body << "}\n";
                    return;
                }
                if (over.type == CType::Vec) {
                    indent(body, depth);
                    body << "{\n";
                    indent(body, depth + 1);
                    body << "knot_vec _loop_v = " << over.code << ";\n";
                    indent(body, depth + 1);
                    body << "for (int _i = 0; _i < _loop_v.n; ++_i) {\n";
                    indent(body, depth + 2);
                    body << "double " << s.name << " = _loop_v.data[_i];\n";
                    {
                        CScope sub(scope);
                        sub.define(s.name, CType::Num);
                        for (const auto& st : s.body) emit_stmt(*st, &sub, body, depth + 2);
                    }
                    indent(body, depth + 1);
                    body << "}\n";
                    indent(body, depth);
                    body << "}\n";
                    return;
                }
                fail(s.span, "loop over unsupported type");
            }
            case StmtKind::For: {
                ExprResult bound = emit_expr(*s.expr, scope);
                switch (s.for_form) {
                    case ForForm::ToCount: {
                        if (bound.type != CType::Num)
                            fail(s.expr->span,
                                std::string("'for ... to' needs a num bound, got ")
                                + ctype_name(bound.type));
                        indent(body, depth);
                        body << "{\n";
                        indent(body, depth + 1);
                        body << "int _for_n = (int)(" << bound.code << ");\n";
                        indent(body, depth + 1);
                        body << "for (int " << s.name << " = 0; "
                             << s.name << " < _for_n; ++" << s.name << ") {\n";
                        {
                            CScope sub(scope);
                            sub.define(s.name, CType::Num);
                            for (const auto& st : s.body)
                                emit_stmt(*st, &sub, body, depth + 2);
                        }
                        indent(body, depth + 1); body << "}\n";
                        indent(body, depth);     body << "}\n";
                        return;
                    }
                    case ForForm::InElem: {
                        if (bound.type != CType::Vec)
                            fail(s.expr->span,
                                std::string("'for ... in' needs a vec, got ")
                                + ctype_name(bound.type));
                        indent(body, depth);
                        body << "{\n";
                        indent(body, depth + 1);
                        body << "knot_vec _for_v = " << bound.code << ";\n";
                        indent(body, depth + 1);
                        body << "for (int _for_i = 0; _for_i < _for_v.n; ++_for_i) {\n";
                        indent(body, depth + 2);
                        body << "double " << s.name << " = _for_v.data[_for_i];\n";
                        {
                            CScope sub(scope);
                            sub.define(s.name, CType::Num);
                            for (const auto& st : s.body)
                                emit_stmt(*st, &sub, body, depth + 2);
                        }
                        indent(body, depth + 1); body << "}\n";
                        indent(body, depth);     body << "}\n";
                        return;
                    }
                    case ForForm::InBoth: {
                        if (bound.type != CType::Vec)
                            fail(s.expr->span,
                                std::string("'for i, x in' needs a vec, got ")
                                + ctype_name(bound.type));
                        indent(body, depth);
                        body << "{\n";
                        indent(body, depth + 1);
                        body << "knot_vec _for_v = " << bound.code << ";\n";
                        indent(body, depth + 1);
                        body << "for (int " << s.name
                             << " = 0; " << s.name << " < _for_v.n; ++"
                             << s.name << ") {\n";
                        indent(body, depth + 2);
                        body << "double " << s.elem_name
                             << " = _for_v.data[" << s.name << "];\n";
                        {
                            CScope sub(scope);
                            sub.define(s.name,      CType::Num);
                            sub.define(s.elem_name, CType::Num);
                            for (const auto& st : s.body)
                                emit_stmt(*st, &sub, body, depth + 2);
                        }
                        indent(body, depth + 1); body << "}\n";
                        indent(body, depth);     body << "}\n";
                        return;
                    }
                }
                fail(s.span, "internal: unhandled for-form");
            }
            case StmtKind::Block: {
                indent(body, depth);
                body << "{\n";
                CScope sub(scope);
                for (const auto& st : s.body) emit_stmt(*st, &sub, body, depth + 1);
                indent(body, depth);
                body << "}\n";
                return;
            }
            case StmtKind::FnDecl: {
                // Functions are emitted at top-level (see emit_fn). If we hit
                // one here, it's a nested def, which v1 doesn't support.
                fail(s.span, "nested function definitions not supported in --cc v1");
            }
            case StmtKind::TestDecl: {
                // Test blocks are only meaningful under --test (--interp).
                // Under --exec they compile to nothing, the same as a
                // suppressed dead-code branch. The body still has to be
                // well-formed knot, but it isn't lowered.
                return;
            }
            case StmtKind::Narrate: {
                // Evaluate the expression and print "# <text>\n".
                // We emit the narrate-enabled check inline so that
                // --no-narrate at runtime suppresses output without
                // requiring a recompile. The runtime exposes an int
                // flag for this.
                ExprResult r = emit_expr(*s.expr, scope);
                indent(body, depth);
                body << "if (knot_narration_on) { fputs(\"# \", stdout); ";
                switch (r.type) {
                    case CType::Num:  body << "knot_print_num("  << r.code << ");"; break;
                    case CType::Bool: body << "fputs((" << r.code << ")?\"true\":\"false\", stdout);"; break;
                    case CType::Str:  body << "knot_print_str("  << r.code << ");"; break;
                    case CType::Vec:  body << "knot_print_vec("  << r.code << ");"; break;
                    case CType::Mat:  body << "knot_print_mat("  << r.code << ");"; break;
                    default: fail(s.span, "narrate: unsupported expression type");
                }
                body << " knot_print_newline(); }\n";
                return;
            }
            case StmtKind::Show: {
                if (s.show_exprs.empty()) {
                    indent(body, depth);
                    body << "knot_print_newline();\n";
                    return;
                }
                for (size_t i = 0; i < s.show_exprs.size(); ++i) {
                    ExprResult r = emit_expr(*s.show_exprs[i], scope);
                    const std::string& label =
                        (i < s.show_labels.size() ? s.show_labels[i] : "<expr>");
                    // Escape backslash and double-quote for embedding
                    // in a C string literal. Other characters pass
                    // through; labels are user-typed knot source so
                    // unprintable bytes are not a realistic concern.
                    std::string esc;
                    esc.reserve(label.size() + 2);
                    for (char c : label) {
                        if (c == '\\' || c == '"') esc += '\\';
                        esc += c;
                    }
                    indent(body, depth);
                    body << "fputs(\"" << esc << ": \", stdout); ";
                    switch (r.type) {
                        case CType::Num:  body << "knot_print_num("  << r.code << ");"; break;
                        case CType::Bool: body << "fputs((" << r.code << ")?\"true\":\"false\", stdout);"; break;
                        case CType::Str:  body << "knot_print_str("  << r.code << ");"; break;
                        case CType::Vec:  body << "knot_print_vec("  << r.code << ");"; break;
                        case CType::Mat:  body << "knot_print_mat("  << r.code << ");"; break;
                        default: fail(s.span, "show: unsupported type");
                    }
                    body << " knot_print_newline();\n";
                }
                return;
            }
        }
    }

    // Emit a top-level function definition. Param types and the return
    // type are inferred from the body.
    // First-pass: infer and register the function's signature without
    // emitting its body. Idempotent — safe to call multiple times to
    // refine inference based on other functions' sigs being available.
    void register_fn(const Stmt& s) {
        FnSig sig;
        for (const auto& p : s.params) {
            sig.param_types.push_back(infer_param_type(p, s.body));
        }
        sig.param_defaults.resize(s.params.size(), nullptr);
        for (size_t i = 0; i < s.param_defaults.size() && i < s.params.size(); ++i) {
            sig.param_defaults[i] = s.param_defaults[i].get();
        }
        sig.ret_type = CType::Num;
        // Sniff a return type.
        CScope sniff_scope;
        for (size_t i = 0; i < s.params.size(); ++i)
            sniff_scope.define(s.params[i], sig.param_types[i]);
        fn_sigs_[s.name] = sig;
        find_return_type(s.body, &sniff_scope, sig);
        fn_sigs_[s.name] = sig;
    }

    void emit_fn(const Stmt& s, CScope* global_scope) {
        // Signature already registered; just emit the body.
        auto it = fn_sigs_.find(s.name);
        if (it == fn_sigs_.end()) {
            register_fn(s);
            it = fn_sigs_.find(s.name);
        }
        const FnSig& sig = it->second;

        fn_defs_ << ctype_name(sig.ret_type) << " " << s.name << "(";
        for (size_t i = 0; i < s.params.size(); ++i) {
            if (i) fn_defs_ << ", ";
            fn_defs_ << c_decl(sig.param_types[i], s.params[i]);
        }
        fn_defs_ << ") {\n";

        CScope body_scope(global_scope);
        for (size_t i = 0; i < s.params.size(); ++i) {
            body_scope.define(s.params[i], sig.param_types[i]);
        }
        for (const auto& st : s.body) {
            emit_stmt(*st, &body_scope, fn_defs_, 1);
        }
        fn_defs_ << "}\n\n";
    }

    // Emit (once) a thunk that adapts a bare `def NAME(...)` to the
    // closure-fat-pointer ABI: the thunk takes a void* env (ignored)
    // plus the user-visible doubles, and forwards to NAME. Returns
    // the thunk's C identifier. Writes to prelude_ so we don't
    // interrupt whatever function body is currently being streamed
    // to fn_defs_.
    std::string emit_def_thunk(const std::string& def_name, int arity) {
        std::string thunk_name = "__knot_thunk_" + def_name
                               + "_a" + std::to_string(arity);
        auto key = std::make_pair(def_name, arity);
        if (emitted_thunks_.count(key)) return thunk_name;
        emitted_thunks_.insert(key);
        prelude_ << "static double " << thunk_name
                 << "(void* __env";
        for (int i = 0; i < arity; ++i) prelude_ << ", double __a" << i;
        prelude_ << ") {\n    (void)__env;\n    return " << def_name << "(";
        for (int i = 0; i < arity; ++i) {
            if (i) prelude_ << ", ";
            prelude_ << "__a" << i;
        }
        prelude_ << ");\n}\n\n";
        return thunk_name;
    }

    // Emit a lifted top-level C function for `fn(x[, y, z]) -> EXPR`,
    // plus the env struct that captures its free variables. Returns
    // a knot_clos_X_X struct literal that wraps the lifted fn pointer
    // and a heap-allocated env. v1 leaks the env -- numerical scripts
    // run and terminate; arena allocation is v2.
    ExprResult emit_fnexpr(const Expr& e, CScope* scope) {
        if (!e.lhs)
            fail(e.span, "FnExpr without body");
        size_t arity = e.params.size();
        if (arity < 1 || arity > 3)
            fail(e.span, "fn(...) must take 1, 2 or 3 args in --exec");

        // Free vars: idents in the body that aren't params or known fns.
        std::vector<std::string> bound = e.params;
        std::vector<std::string> frees;
        Parser::collect_free_vars(*e.lhs, bound, frees);

        // Each captured var must already have a type in the surrounding
        // scope (for now we only support num captures; lifting matrices
        // or vecs would require copying the struct fields by value, which
        // is fine for the value types but unwise for vec/mat -- their
        // contents live on the heap and the env holding a borrowed copy
        // is fine, but the codegen below is num-only).
        std::vector<std::pair<std::string, CType>> capture_info;
        capture_info.reserve(frees.size());
        for (const auto& f : frees) {
            CType ft = scope->lookup(f);
            if (ft == CType::Unknown) {
                // Maybe a user-defined function or builtin; collect_free_vars
                // is supposed to filter those, but in case it didn't, skip.
                continue;
            }
            if (ft != CType::Num) {
                fail(e.span,
                     "captured variable '" + f + "' has non-num type ("
                     + ctype_name(ft) + "); only num captures supported");
            }
            capture_info.push_back({f, ft});
        }

        int id = next_fnexpr_id_++;
        std::string fn_name  = "__knot_fnexpr_" + std::to_string(id);
        std::string env_name = "__knot_env_"    + std::to_string(id);
        CType ct = arity == 1 ? CType::FnD_D
                 : arity == 2 ? CType::FnDD_D
                 :              CType::FnDDD_D;
        std::string clos_t = ctype_name(ct);

        // Emit env struct (or a 1-byte placeholder if there are no
        // captures -- can't have a zero-size struct in standard C).
        prelude_ << "struct " << env_name << " {";
        if (capture_info.empty()) {
            prelude_ << " char __pad;";
        } else {
            for (const auto& cap : capture_info) {
                prelude_ << " double " << cap.first << ";";
            }
        }
        prelude_ << " };\n";

        // Emit the lifted function. Inside the body we declare locals
        // (named to match the captures) initialized from env, so the
        // emitter can use the param names verbatim when emitting the
        // body expression.
        prelude_ << "static double " << fn_name << "(void* __env_raw";
        for (size_t i = 0; i < arity; ++i)
            prelude_ << ", double " << e.params[i];
        prelude_ << ") {\n";
        prelude_ << "    struct " << env_name << "* __env = "
                 << "(struct " << env_name << "*)__env_raw;\n";
        if (capture_info.empty()) {
            prelude_ << "    (void)__env;\n";
        }
        // The body emission needs to see params and captures as Num.
        CScope body_scope; // top-level, captures + params only
        for (size_t i = 0; i < arity; ++i)
            body_scope.define(e.params[i], CType::Num);
        for (const auto& cap : capture_info) {
            body_scope.define(cap.first, CType::Num);
            prelude_ << "    double " << cap.first
                     << " = __env->" << cap.first << ";\n";
        }
        ExprResult body_res = emit_expr(*e.lhs, &body_scope);
        if (body_res.type != CType::Num)
            fail(e.span, "fn(...) -> EXPR must return num (got "
                + std::string(ctype_name(body_res.type)) + ")");
        prelude_ << "    return " << body_res.code << ";\n";
        prelude_ << "}\n\n";

        // At the call site, build the closure literal: function pointer
        // plus heap-allocated env. We use a GCC statement-expression to
        // pack the alloc + initialization + cast into one expression.
        std::ostringstream o;
        o << "((" << clos_t << "){" << fn_name << ", ";
        if (capture_info.empty()) {
            o << "NULL";
        } else {
            o << "({ struct " << env_name << "* __e = "
              << "(struct " << env_name
              << "*)malloc(sizeof(*__e));";
            for (const auto& cap : capture_info) {
                o << " __e->" << cap.first << " = "
                  << cap.first << ";";
            }
            o << " (void*)__e; })";
        }
        o << "})";
        return {o.str(), ct};
    }

    // Emit a C parameter or local declaration: "TYPE NAME".
    static std::string c_decl(CType t, const std::string& name) {
        return std::string(ctype_name(t)) + " " + name;
    }
    static int ctype_rank(CType t) {
        switch (t) {
            case CType::Unknown: return 0;
            case CType::Nil:     return 0;
            case CType::Bool:    return 1;
            case CType::Str:     return 1;
            case CType::Num:     return 1;
            case CType::Vec:     return 2;
            case CType::Mat:     return 2;
            case CType::FnD_D:   return 2;
            case CType::FnDD_D:  return 2;
            case CType::FnDDD_D: return 2;
        }
        return 0;
    }
    static CType ctype_max(CType a, CType b) {
        return ctype_rank(a) >= ctype_rank(b) ? a : b;
    }
    bool find_return_type(const std::vector<StmtPtr>& body, CScope* scope, FnSig& sig) {
        for (const auto& st : body) {
            switch (st->kind) {
                case StmtKind::Assign: {
                    if (st->target) continue;
                    try {
                        ExprResult r = emit_expr(*st->expr, scope);
                        scope->define(st->name, r.type);
                    } catch (...) {}
                    break;
                }
                case StmtKind::Return: {
                    if (!st->expr) return true;
                    try {
                        ExprResult r = emit_expr(*st->expr, scope);
                        sig.ret_type = r.type;
                        return true;
                    } catch (...) {
                        return false;
                    }
                }
                case StmtKind::If: {
                    CScope then_scope(scope);
                    if (find_return_type(st->body, &then_scope, sig)) return true;
                    CScope else_scope(scope);
                    if (find_return_type(st->else_body, &else_scope, sig)) return true;
                    break;
                }
                case StmtKind::While:
                case StmtKind::Loop: {
                    CScope sub(scope);
                    if (st->kind == StmtKind::Loop) sub.define(st->name, CType::Num);
                    if (find_return_type(st->body, &sub, sig)) return true;
                    break;
                }
                case StmtKind::For: {
                    CScope sub(scope);
                    // Whatever the form, the index/elem binders are Num.
                    sub.define(st->name, CType::Num);
                    if (st->for_form == ForForm::InBoth) {
                        sub.define(st->elem_name, CType::Num);
                    }
                    if (find_return_type(st->body, &sub, sig)) return true;
                    break;
                }
                case StmtKind::Block: {
                    CScope sub(scope);
                    if (find_return_type(st->body, &sub, sig)) return true;
                    break;
                }
                default: break;
            }
        }
        return false;
    }

    // Scan body for usages of `name` that constrain its type. Returns the
    // most-specific type discovered, or CType::Num as default.
    CType infer_param_type(const std::string& name,
                           const std::vector<StmtPtr>& body) {
        CType found = CType::Num;
        for (const auto& st : body) {
            CType t = scan_stmt_for_type(name, *st);
            if (t != CType::Num) found = t;
        }
        return found;
    }

    // Does this body reference an identifier named `name` anywhere?
    bool body_uses_name(const std::vector<StmtPtr>& body, const std::string& name) {
        for (const auto& st : body) {
            if (stmt_uses_name(*st, name)) return true;
        }
        return false;
    }
    bool stmt_uses_name(const Stmt& s, const std::string& name) {
        if (s.expr   && expr_uses_name(*s.expr,   name)) return true;
        if (s.target && expr_uses_name(*s.target, name)) return true;
        for (const auto& d : s.param_defaults) if (d && expr_uses_name(*d, name)) return true;
        if (body_uses_name(s.body,      name)) return true;
        if (body_uses_name(s.else_body, name)) return true;
        return false;
    }
    bool expr_uses_name(const Expr& e, const std::string& name) {
        switch (e.kind) {
            case ExprKind::Ident: return e.str == name;
            case ExprKind::Binary:
                if (e.lhs && expr_uses_name(*e.lhs, name)) return true;
                if (e.rhs && expr_uses_name(*e.rhs, name)) return true;
                return false;
            case ExprKind::Unary:
                return e.rhs && expr_uses_name(*e.rhs, name);
            case ExprKind::Call:
            case ExprKind::Index:
                if (e.callee && expr_uses_name(*e.callee, name)) return true;
                // fallthrough
            case ExprKind::VecLit:
                for (const auto& el : e.elems)
                    if (expr_uses_name(*el, name)) return true;
                return false;
            default: return false;
        }
    }

    CType scan_stmt_for_type(const std::string& name, const Stmt& s) {
        CType t = CType::Num;
        // `loop NAME { ... it ... }` -- if the body actually uses the
        // implicit `it` (or whatever name was assigned via `as`), NAME is
        // being iterated *as a vec*. Bare `loop max_iter { ... }` without
        // referencing `it` is a count, no hint.
        if (s.kind == StmtKind::Loop && s.expr
         && s.expr->kind == ExprKind::Ident && s.expr->str == name) {
            const std::string& iter_var = s.name;  // "it" by default
            if (body_uses_name(s.body, iter_var)) {
                t = ctype_max(t, CType::Vec);
            }
        }
        // `for ... in NAME` is an unambiguous Vec hint. `for i to NAME` is
        // a Num hint (which is the default anyway).
        if (s.kind == StmtKind::For && s.expr
         && s.expr->kind == ExprKind::Ident && s.expr->str == name) {
            if (s.for_form == ForForm::InElem || s.for_form == ForForm::InBoth) {
                t = ctype_max(t, CType::Vec);
            }
        }
        if (s.expr)   t = ctype_max(t, scan_expr_for_type(name, *s.expr));
        if (s.target) t = ctype_max(t, scan_expr_for_type(name, *s.target));
        for (const auto& d : s.param_defaults)
            if (d) t = ctype_max(t, scan_expr_for_type(name, *d));
        for (const auto& b : s.body)
            t = ctype_max(t, scan_stmt_for_type(name, *b));
        for (const auto& b : s.else_body)
            t = ctype_max(t, scan_stmt_for_type(name, *b));
        return t;
    }

    CType scan_expr_for_type(const std::string& name, const Expr& e) {
        CType t = CType::Num;
        switch (e.kind) {
            case ExprKind::Call:
                // If `name` is itself being called -- f(...) -- it's a function.
                if (e.callee && e.callee->kind == ExprKind::Ident
                 && e.callee->str == name) {
                    // Numerical signatures of arity 1..3 -- covers bisect /
                    // newton / simpson / trapezoid / golden_section (1-arg)
                    // and rk4 (2-arg), with headroom for 3-arg callbacks.
                    if (e.elems.size() == 1) return CType::FnD_D;
                    if (e.elems.size() == 2) return CType::FnDD_D;
                    if (e.elems.size() == 3) return CType::FnDDD_D;
                }
                if (e.callee && e.callee->kind == ExprKind::Ident) {
                    const std::string& fn = e.callee->str;
                    for (size_t i = 0; i < e.elems.size(); ++i) {
                        const auto& a = e.elems[i];
                        if (a->kind == ExprKind::Ident && a->str == name) {
                            // Builtin hints.
                            if (fn == "rows" || fn == "cols") return CType::Mat;
                            if (fn == "len") return CType::Vec;
                            if (fn == "norm" || fn == "dot")  return CType::Vec;
                            if (fn == "transpose") return CType::Mat;
                            // at/set: container is in position 0.
                            // (vec, i)  / (vec, i, v)     => arg 0 is Vec
                            // (mat, i, j) / (mat, i, j, v)=> arg 0 is Mat
                            if ((fn == "at" || fn == "set") && i == 0) {
                                size_t nargs = e.elems.size();
                                if (fn == "at"  && nargs == 2) return CType::Vec;
                                if (fn == "at"  && nargs == 3) return CType::Mat;
                                if (fn == "set" && nargs == 3) return CType::Vec;
                                if (fn == "set" && nargs == 4) return CType::Mat;
                            }
                            // User-defined function hint: if we already
                            // know the signature, use it.
                            auto it = fn_sigs_.find(fn);
                            if (it != fn_sigs_.end()
                             && i < it->second.param_types.size()) {
                                CType pt = it->second.param_types[i];
                                if (pt != CType::Num) return pt;
                            }
                        }
                    }
                }
                if (e.callee) t = ctype_max(t, scan_expr_for_type(name, *e.callee));
                for (const auto& a : e.elems) t = ctype_max(t, scan_expr_for_type(name, *a));
                break;
            case ExprKind::Index:
                if (e.callee && e.callee->kind == ExprKind::Ident && e.callee->str == name) {
                    if (e.elems.size() == 1) return CType::Vec;
                    if (e.elems.size() == 2) return CType::Mat;
                }
                if (e.callee) t = ctype_max(t, scan_expr_for_type(name, *e.callee));
                for (const auto& a : e.elems) t = ctype_max(t, scan_expr_for_type(name, *a));
                break;
            case ExprKind::Binary:
                if (e.lhs) t = ctype_max(t, scan_expr_for_type(name, *e.lhs));
                if (e.rhs) t = ctype_max(t, scan_expr_for_type(name, *e.rhs));
                break;
            case ExprKind::Unary:
                if (e.rhs) t = ctype_max(t, scan_expr_for_type(name, *e.rhs));
                break;
            case ExprKind::VecLit:
                for (const auto& el : e.elems) t = ctype_max(t, scan_expr_for_type(name, *el));
                break;
            default: break;
        }
        return t;
    }
};

} // namespace knot
