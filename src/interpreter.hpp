#pragma once
#include "annotations.hpp"
#include "ast.hpp"
#include "diag.hpp"
#include "hash.hpp"
#include "linalg.hpp"
#include "value.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>

namespace knot {

// We use exceptions for control flow on `return`. Cheap and clear for an
// interpreter this small; not idiomatic for production-grade VMs.
struct ReturnSignal {
    Value value;
};

class Interpreter {
public:
    Interpreter() {
        globals = std::make_shared<Env>();
        register_builtins();
    }

    void run(const std::vector<StmtPtr>& program) {
        // The single source of truth for stmt-block execution -- including
        // step-mode lookup, composite-key matching, and per-key suppression
        // -- lives in run_block(). The top-level program is just another
        // block.
        run_block(program, globals);
    }

    // Step-mode controls. Set before calling run().
    void enable_step_mode(Annotations a) {
        step_mode = true;
        annotations = std::move(a);
    }

    // Exposed for the REPL: evaluate one expression in globals.
    Value eval_in_globals(const Expr& e) { return eval(e, globals); }

    // For nice error printing from main().
    EnvPtr global_env() const { return globals; }

private:
    EnvPtr globals;
    bool step_mode = false;
    Annotations annotations;
    // Keys that have already been shown this run. Used to suppress duplicate
    // annotation printing in tight loops.
    std::unordered_set<std::string> seen_annotations;

    // Print one annotation line to stderr, unless we've already shown this
    // exact one earlier in the run. The * prefix opts out of suppression
    // (use it on an annotation you actually want to see every iteration of
    // a tight loop).
    void print_annotation(const std::string& key, const std::string& desc) {
        if (!desc.empty() && desc[0] == '*') {
            // Verbose: print every time, but strip the leading * marker.
            std::cerr << "# " << desc.substr(1) << "\n";
            return;
        }
        if (!seen_annotations.insert(key).second) return;
        std::cerr << "# " << desc << "\n";
    }

    // Step-aware block runner. Use this anywhere a block of stmts gets
    // executed (function bodies, if/else branches, loop bodies). Falls
    // through to plain exec() when step_mode is off.
    void run_block(const std::vector<StmtPtr>& body, EnvPtr env) {
        if (!step_mode || annotations.empty()) {
            for (const auto& s : body) exec(*s, env);
            return;
        }
        std::vector<std::string> hashes;
        hashes.reserve(body.size());
        for (const auto& s : body) hashes.push_back(stmt_hash(*s));
        size_t i = 0;
        while (i < body.size()) {
            auto hit = annotations.lookup(hashes, i);
            if (hit.consumed > 0) {
                // Build the key the same way the annotation file uses it.
                std::string key = hashes[i];
                for (int k = 1; k < hit.consumed; ++k) {
                    key += "+";
                    key += hashes[i + k];
                }
                print_annotation(key, hit.desc);
                for (int k = 0; k < hit.consumed; ++k) exec(*body[i + k], env);
                i += hit.consumed;
            } else {
                exec(*body[i], env);
                ++i;
            }
        }
    }

    // ---- Statements ------------------------------------------------------

    void exec(const Stmt& s, EnvPtr env) {
        switch (s.kind) {
            case StmtKind::Assign: {
                Value v = eval(*s.expr, env);
                if (s.target) {
                    assign_index(*s.target, std::move(v), env);
                } else {
                    // Auto-create-or-reassign. If the name exists anywhere
                    // in the scope chain, update in place; otherwise create
                    // in the current scope. This is Python-like.
                    if (!env->assign(s.name, v)) {
                        env->define(s.name, std::move(v));
                    }
                }
                return;
            }
            case StmtKind::CompAssign: {
                Value rhs = eval(*s.expr, env);
                if (s.target) {
                    // Compound on indexed target: read, op, write.
                    Value cur = eval(*s.target, env);
                    Value nv = apply_binop(s.comp_op, cur, rhs, s.span);
                    assign_index(*s.target, std::move(nv), env);
                } else {
                    Value* cur = env->find(s.name);
                    if (!cur) throw Diag(s.span,
                        "compound assignment to undefined variable '" + s.name + "'");
                    Value nv = apply_binop(s.comp_op, *cur, rhs, s.span);
                    env->assign(s.name, std::move(nv));
                }
                return;
            }
            case StmtKind::ExprStmt: {
                (void)eval(*s.expr, env);
                return;
            }
            case StmtKind::Print: {
                if (!s.expr) { std::cout << "\n"; return; }
                Value v = eval(*s.expr, env);
                std::cout << format_value(v) << "\n";
                return;
            }
            case StmtKind::If: {
                Value c = eval(*s.expr, env);
                if (truthy(c, s.expr->span)) {
                    auto sub = std::make_shared<Env>(env);
                    run_block(s.body, sub);
                } else if (!s.else_body.empty()) {
                    auto sub = std::make_shared<Env>(env);
                    run_block(s.else_body, sub);
                }
                return;
            }
            case StmtKind::While: {
                while (true) {
                    Value c = eval(*s.expr, env);
                    if (!truthy(c, s.expr->span)) break;
                    auto sub = std::make_shared<Env>(env);
                    run_block(s.body, sub);
                }
                return;
            }
            case StmtKind::Loop: {
                Value over = eval(*s.expr, env);
                if (over.is_num()) {
                    int n = (int)over.as_num();
                    if (n < 0)
                        throw Diag(s.expr->span,
                            "loop bound must be non-negative, got " + std::to_string(n));
                    for (int it = 0; it < n; ++it) {
                        auto sub = std::make_shared<Env>(env);
                        if (over.tag) {
                            ShapeTag t = *over.tag;
                            sub->define(s.name, Value::tagged_num((double)it, t));
                        } else {
                            sub->define(s.name, Value::num((double)it));
                        }
                        run_block(s.body, sub);
                    }
                } else if (over.is_vec()) {
                    const Vec& vv = over.as_vec();
                    for (size_t i = 0; i < vv.size(); ++i) {
                        auto sub = std::make_shared<Env>(env);
                        sub->define(s.name, Value::num(vv[i]));
                        run_block(s.body, sub);
                    }
                } else {
                    throw Diag(s.expr->span,
                        std::string("can't loop over ") + over.type_name()
                        + " (need num or vec)");
                }
                return;
            }
            case StmtKind::For: {
                Value bound = eval(*s.expr, env);
                switch (s.for_form) {
                    case ForForm::ToCount: {
                        if (!bound.is_num())
                            throw Diag(s.expr->span,
                                std::string("'for ... to' needs a num bound, got ")
                                + bound.type_name());
                        int n = (int)bound.as_num();
                        if (n < 0)
                            throw Diag(s.expr->span,
                                "for-to bound must be non-negative, got "
                                + std::to_string(n));
                        for (int it = 0; it < n; ++it) {
                            auto sub = std::make_shared<Env>(env);
                            if (bound.tag) {
                                sub->define(s.name,
                                    Value::tagged_num((double)it, *bound.tag));
                            } else {
                                sub->define(s.name, Value::num((double)it));
                            }
                            run_block(s.body, sub);
                        }
                        break;
                    }
                    case ForForm::InElem: {
                        if (!bound.is_vec())
                            throw Diag(s.expr->span,
                                std::string("'for ... in' needs a vec, got ")
                                + bound.type_name());
                        const Vec& vv = bound.as_vec();
                        for (size_t i = 0; i < vv.size(); ++i) {
                            auto sub = std::make_shared<Env>(env);
                            sub->define(s.name, Value::num(vv[i]));
                            run_block(s.body, sub);
                        }
                        break;
                    }
                    case ForForm::InBoth: {
                        if (!bound.is_vec())
                            throw Diag(s.expr->span,
                                std::string("'for i, x in' needs a vec, got ")
                                + bound.type_name());
                        const Vec& vv = bound.as_vec();
                        for (size_t i = 0; i < vv.size(); ++i) {
                            auto sub = std::make_shared<Env>(env);
                            sub->define(s.name,      Value::num((double)i));
                            sub->define(s.elem_name, Value::num(vv[i]));
                            run_block(s.body, sub);
                        }
                        break;
                    }
                }
                return;
            }
            case StmtKind::Block: {
                auto sub = std::make_shared<Env>(env);
                run_block(s.body, sub);
                return;
            }
            case StmtKind::FnDecl: {
                auto f = std::make_shared<Function>();
                f->params = s.params;
                f->body = &s.body;
                f->closure = env;
                f->name = s.name;
                f->param_defaults = &s.param_defaults;
                // If the body starts with a bare string-literal expression
                // statement, treat it as the function's docstring (Python
                // convention). Surfaces in the REPL via `whatis NAME`.
                if (!s.body.empty()
                 && s.body[0]->kind == StmtKind::ExprStmt
                 && s.body[0]->expr
                 && s.body[0]->expr->kind == ExprKind::StringLit) {
                    f->docstring = s.body[0]->expr->str;
                }
                env->define(s.name, Value::fn(f));
                return;
            }
            case StmtKind::Return: {
                ReturnSignal r;
                r.value = s.expr ? eval(*s.expr, env) : Value::nil();
                throw r;
            }
        }
    }

    void assign_index(const Expr& target, Value val, EnvPtr env) {
        Value container = eval(*target.callee, env);
        if (container.is_vec()) {
            if (target.elems.size() != 1)
                throw Diag(target.span, "vector index requires 1 integer");
            if (target.elems[0]->kind == ExprKind::Slice)
                throw Diag(target.span, "slice assignment is not supported yet");
            auto& sp = std::get<std::shared_ptr<Vec>>(container.v);
            Value iv = eval(*target.elems[0], env);
            check_tag(iv, sp->len_tag.get(), "vec index", target.elems[0]->span);
            int i = to_index(iv, target.elems[0]->span);
            i = wrap_index(i, (int)sp->size(), target.elems[0]->span, "vector");
            if (!val.is_num())
                throw Diag(target.span, "assigning non-number into vec element");
            (*sp)[i] = val.as_num();
            return;
        }
        if (container.is_mat()) {
            if (target.elems.size() != 2)
                throw Diag(target.span, "matrix index requires 2 integers");
            auto& sp = std::get<std::shared_ptr<Mat>>(container.v);
            Value iv = eval(*target.elems[0], env);
            Value jv = eval(*target.elems[1], env);
            check_tag(iv, sp->row_tag.get(), "row index", target.elems[0]->span);
            check_tag(jv, sp->col_tag.get(), "col index", target.elems[1]->span);
            int i = to_index(iv, target.elems[0]->span);
            int j = to_index(jv, target.elems[1]->span);
            i = wrap_index(i, (int)sp->rows, target.elems[0]->span, "row");
            j = wrap_index(j, (int)sp->cols, target.elems[1]->span, "col");
            if (!val.is_num())
                throw Diag(target.span, "assigning non-number into mat element");
            sp->at(i, j) = val.as_num();
            return;
        }
        throw Diag(target.callee->span,
            std::string("cannot index-assign into ") + container.type_name());
    }

    // ---- Expressions -----------------------------------------------------

    Value eval(const Expr& e, EnvPtr env) {
        switch (e.kind) {
            case ExprKind::NumberLit: return Value::num(e.num);
            case ExprKind::StringLit: return Value::str(e.str);
            case ExprKind::BoolLit:   return Value::boolean(e.boolean);
            case ExprKind::NilLit:    return Value::nil();
            case ExprKind::Ident: {
                Value* v = env->find(e.str);
                if (!v) throw Diag(e.span,
                    "undefined variable '" + e.str + "'");
                return *v;
            }
            case ExprKind::VecLit: {
                // First evaluate all elements. If they're all numbers, build
                // a numerical Vec. Otherwise build a heterogeneous List.
                std::vector<Value> vals;
                vals.reserve(e.elems.size());
                bool all_num = true;
                for (const auto& el : e.elems) {
                    vals.push_back(eval(*el, env));
                    if (!vals.back().is_num()) all_num = false;
                }
                // Empty `[]` is an empty list, not an empty vec -- so it
                // composes with append(). A 0-length numerical vec has no
                // useful operations on it anyway.
                if (all_num && !vals.empty()) {
                    Vec out(vals.size());
                    for (size_t i = 0; i < vals.size(); ++i) out[i] = vals[i].as_num();
                    return Value::vec(std::move(out));
                }
                return Value::list(std::move(vals));
            }
            case ExprKind::MatLit: {
                size_t r = e.rows.size();
                size_t c = r ? e.rows[0].size() : 0;
                Mat out(r, c);
                for (size_t i = 0; i < r; ++i) {
                    if (e.rows[i].size() != c)
                        throw Diag(e.span,
                            "matrix row " + std::to_string(i)
                            + " has " + std::to_string(e.rows[i].size())
                            + " elements, expected " + std::to_string(c));
                    for (size_t j = 0; j < c; ++j) {
                        Value v = eval(*e.rows[i][j], env);
                        if (!v.is_num())
                            throw Diag(e.rows[i][j]->span,
                                std::string("matrix element must be num, got ")
                                + v.type_name());
                        out.at(i, j) = v.as_num();
                    }
                }
                return Value::mat(std::move(out));
            }
            case ExprKind::Unary: {
                Value r = eval(*e.rhs, env);
                if (e.unop == UnOp::Neg) {
                    if (r.is_num()) return Value::num(-r.as_num());
                    if (r.is_vec()) return Value::vec(vec_scale(r.as_vec(), -1.0));
                    if (r.is_mat()) return Value::mat(mat_scale(r.as_mat(), -1.0));
                    throw Diag(e.span,
                        std::string("unary '-' not defined for ") + r.type_name());
                } else {
                    return Value::boolean(!truthy(r, e.rhs->span));
                }
            }
            case ExprKind::Binary: return eval_binary(e, env);
            case ExprKind::Index:  return eval_index(e, env);
            case ExprKind::Call:   return eval_call(e, env);
            case ExprKind::Slice:
                throw Diag(e.span, "slice syntax 'a:b' only valid inside '[]'");
        }
        return Value::nil();
    }

    Value eval_binary(const Expr& e, EnvPtr env) {
        // Short-circuit logical ops first.
        if (e.binop == BinOp::And) {
            Value l = eval(*e.lhs, env);
            if (!truthy(l, e.lhs->span)) return Value::boolean(false);
            Value r = eval(*e.rhs, env);
            return Value::boolean(truthy(r, e.rhs->span));
        }
        if (e.binop == BinOp::Or) {
            Value l = eval(*e.lhs, env);
            if (truthy(l, e.lhs->span)) return Value::boolean(true);
            Value r = eval(*e.rhs, env);
            return Value::boolean(truthy(r, e.rhs->span));
        }

        Value l = eval(*e.lhs, env);
        Value r = eval(*e.rhs, env);

        if (e.binop == BinOp::Eq || e.binop == BinOp::Ne) {
            bool eq = values_equal(l, r);
            return Value::boolean(e.binop == BinOp::Eq ? eq : !eq);
        }

        if (e.binop == BinOp::Lt || e.binop == BinOp::Le
         || e.binop == BinOp::Gt || e.binop == BinOp::Ge) {
            if (!l.is_num() || !r.is_num())
                throw Diag(e.span,
                    std::string("comparison requires numbers, got ")
                    + l.type_name() + " and " + r.type_name());
            double a = l.as_num(), b = r.as_num();
            switch (e.binop) {
                case BinOp::Lt: return Value::boolean(a < b);
                case BinOp::Le: return Value::boolean(a <= b);
                case BinOp::Gt: return Value::boolean(a > b);
                case BinOp::Ge: return Value::boolean(a >= b);
                default: break;
            }
        }

        return apply_binop(e.binop, l, r, e.span);
    }

    // The arithmetic core, factored out so compound assignment can reuse it.
    Value apply_binop(BinOp op, const Value& l, const Value& r, Span s) {
        // Tag propagation: when both sides are numbers and the result is a
        // number, propagate the tag if there's no conflict.  Same tag wins;
        // tag + untagged keeps the tag; conflicting tags drop both.
        auto propagate_tag = [&](double result) -> Value {
            Value v = Value::num(result);
            std::shared_ptr<ShapeTag> lt = l.is_num() ? l.tag : nullptr;
            std::shared_ptr<ShapeTag> rt = r.is_num() ? r.tag : nullptr;
            if (lt && !rt) v.tag = lt;
            else if (rt && !lt) v.tag = rt;
            else if (lt && rt) {
                if (lt->container_id == rt->container_id && lt->axis == rt->axis)
                    v.tag = lt;
                // else: drop both (untagged result)
            }
            return v;
        };
        try {
            switch (op) {
                case BinOp::Add:
                    if (l.is_num() && r.is_num()) return propagate_tag(l.as_num() + r.as_num());
                    if (l.is_vec() && r.is_vec()) return Value::vec(vec_add(l.as_vec(), r.as_vec()));
                    if (l.is_mat() && r.is_mat()) return Value::mat(mat_add(l.as_mat(), r.as_mat()));
                    if (l.is_str() && r.is_str()) return Value::str(l.as_str() + r.as_str());
                    break;
                case BinOp::Sub:
                    if (l.is_num() && r.is_num()) return propagate_tag(l.as_num() - r.as_num());
                    if (l.is_vec() && r.is_vec()) return Value::vec(vec_sub(l.as_vec(), r.as_vec()));
                    if (l.is_mat() && r.is_mat()) return Value::mat(mat_sub(l.as_mat(), r.as_mat()));
                    break;
                case BinOp::Mul:
                    if (l.is_num() && r.is_num()) return propagate_tag(l.as_num() * r.as_num());
                    if (l.is_num() && r.is_vec()) return Value::vec(vec_scale(r.as_vec(), l.as_num()));
                    if (l.is_vec() && r.is_num()) return Value::vec(vec_scale(l.as_vec(), r.as_num()));
                    if (l.is_num() && r.is_mat()) return Value::mat(mat_scale(r.as_mat(), l.as_num()));
                    if (l.is_mat() && r.is_num()) return Value::mat(mat_scale(l.as_mat(), r.as_num()));
                    break;
                case BinOp::Div:
                    if (l.is_num() && r.is_num()) {
                        if (r.as_num() == 0.0) throw Diag(s, "division by zero");
                        return propagate_tag(l.as_num() / r.as_num());
                    }
                    if (l.is_vec() && r.is_num()) return Value::vec(vec_scale(l.as_vec(), 1.0 / r.as_num()));
                    if (l.is_mat() && r.is_num()) return Value::mat(mat_scale(l.as_mat(), 1.0 / r.as_num()));
                    break;
                case BinOp::Mod:
                    if (l.is_num() && r.is_num()) return propagate_tag(std::fmod(l.as_num(), r.as_num()));
                    break;
                case BinOp::Matmul:
                    if (l.is_mat() && r.is_mat()) return Value::mat(mat_mul(l.as_mat(), r.as_mat()));
                    if (l.is_mat() && r.is_vec()) return Value::vec(mat_vec(l.as_mat(), r.as_vec()));
                    if (l.is_vec() && r.is_mat()) return Value::vec(mat_vec(mat_transpose(r.as_mat()), l.as_vec()));
                    if (l.is_vec() && r.is_vec()) return Value::num(vec_dot(l.as_vec(), r.as_vec()));
                    break;
                default: break;
            }
        } catch (const std::runtime_error& ex) {
            throw Diag(s, ex.what());
        }
        throw Diag(s, std::string("operator not defined for ")
            + l.type_name() + " and " + r.type_name());
    }

    Value eval_index(const Expr& e, EnvPtr env) {
        Value container = eval(*e.callee, env);

        if (container.is_vec() && e.elems.size() == 1
         && e.elems[0]->kind == ExprKind::Slice) {
            const Expr& sl = *e.elems[0];
            const Vec& v = container.as_vec();
            int n = (int)v.size();
            int lo = sl.slice_lo ? to_index(eval(*sl.slice_lo, env), sl.slice_lo->span) : 0;
            int hi = sl.slice_hi ? to_index(eval(*sl.slice_hi, env), sl.slice_hi->span) : n;
            if (lo < 0) lo += n;
            if (hi < 0) hi += n;
            if (lo < 0) lo = 0;
            if (hi > n) hi = n;
            if (lo > hi) lo = hi;
            Vec out((size_t)(hi - lo));
            for (int i = lo; i < hi; ++i) out[i - lo] = v[i];
            return Value::vec(std::move(out));
        }

        if (container.is_list()) {
            if (e.elems.size() != 1)
                throw Diag(e.span, "list index requires 1 integer");
            if (e.elems[0]->kind == ExprKind::Slice)
                throw Diag(e.span, "list slicing is not supported yet");
            const ValueList& lst = container.as_list();
            int i = to_index(eval(*e.elems[0], env), e.elems[0]->span);
            i = wrap_index(i, (int)lst.size(), e.elems[0]->span, "list");
            return lst[(size_t)i];
        }

        if (container.is_vec()) {
            if (e.elems.size() != 1)
                throw Diag(e.span,
                    "vector index requires 1 integer, got "
                    + std::to_string(e.elems.size()));
            if (e.elems[0]->kind == ExprKind::Slice) {
                throw Diag(e.span, "internal: unhandled vector slice");
            }
            const auto& sp = std::get<std::shared_ptr<Vec>>(container.v);
            Value iv = eval(*e.elems[0], env);
            check_tag(iv, sp->len_tag.get(), "vec index", e.elems[0]->span);
            int i = to_index(iv, e.elems[0]->span);
            i = wrap_index(i, (int)sp->size(), e.elems[0]->span, "vector");
            return Value::num((*sp)[i]);
        }
        if (container.is_mat()) {
            if (e.elems.size() != 2)
                throw Diag(e.span,
                    "matrix index requires 2 integers, got "
                    + std::to_string(e.elems.size()));
            if (e.elems[0]->kind == ExprKind::Slice
             || e.elems[1]->kind == ExprKind::Slice)
                throw Diag(e.span, "matrix slicing is not supported yet");
            const auto& sp = std::get<std::shared_ptr<Mat>>(container.v);
            Value iv = eval(*e.elems[0], env);
            Value jv = eval(*e.elems[1], env);
            check_tag(iv, sp->row_tag.get(), "row index", e.elems[0]->span);
            check_tag(jv, sp->col_tag.get(), "col index", e.elems[1]->span);
            int i = to_index(iv, e.elems[0]->span);
            int j = to_index(jv, e.elems[1]->span);
            i = wrap_index(i, (int)sp->rows, e.elems[0]->span, "row");
            j = wrap_index(j, (int)sp->cols, e.elems[1]->span, "col");
            return Value::num(sp->at(i, j));
        }
        if (container.is_str()) {
            // Indexing a string returns a 1-char string. Slice produces substr.
            const std::string& sv = container.as_str();
            int n = (int)sv.size();
            if (e.elems.size() == 1 && e.elems[0]->kind == ExprKind::Slice) {
                const Expr& sl = *e.elems[0];
                int lo = sl.slice_lo ? to_index(eval(*sl.slice_lo, env), sl.slice_lo->span) : 0;
                int hi = sl.slice_hi ? to_index(eval(*sl.slice_hi, env), sl.slice_hi->span) : n;
                if (lo < 0) lo += n;
                if (hi < 0) hi += n;
                if (lo < 0) lo = 0;
                if (hi > n) hi = n;
                if (lo > hi) lo = hi;
                return Value::str(sv.substr((size_t)lo, (size_t)(hi - lo)));
            }
            if (e.elems.size() != 1)
                throw Diag(e.span, "string index requires 1 integer");
            int i = to_index(eval(*e.elems[0], env), e.elems[0]->span);
            i = wrap_index(i, n, e.elems[0]->span, "string");
            return Value::str(std::string(1, sv[(size_t)i]));
        }
        throw Diag(e.callee->span,
            std::string("cannot index into ") + container.type_name());
    }

    Value eval_call(const Expr& e, EnvPtr env) {
        Value callee = eval(*e.callee, env);
        std::vector<Value> args;
        args.reserve(e.elems.size());
        for (const auto& a : e.elems) args.push_back(eval(*a, env));

        if (callee.is_builtin()) {
            try {
                return callee.as_builtin().fn(args, e.span);
            } catch (const Diag&) {
                throw;
            } catch (const std::runtime_error& ex) {
                throw Diag(e.span, ex.what());
            }
        }
        if (callee.is_fn()) {
            const Function& f = callee.as_fn();
            size_t nparams = f.params.size();
            if (args.size() > nparams)
                throw Diag(e.span,
                    "function '" + f.name + "' expects at most "
                    + std::to_string(nparams) + " args, got "
                    + std::to_string(args.size()));
            // Required args = those without defaults.
            size_t required = 0;
            if (f.param_defaults) {
                for (const auto& d : *f.param_defaults) {
                    if (!d) ++required;
                    else break; // defaults must be trailing
                }
            } else {
                required = nparams;
            }
            if (args.size() < required)
                throw Diag(e.span,
                    "function '" + f.name + "' expects at least "
                    + std::to_string(required) + " args, got "
                    + std::to_string(args.size()));

            auto frame = std::make_shared<Env>(f.closure);
            for (size_t i = 0; i < nparams; ++i) {
                if (i < args.size()) {
                    frame->define(f.params[i], args[i]);
                } else {
                    // Evaluate default in the function's closure scope (where
                    // the default expression was lexically defined).
                    Value dv = eval(*(*f.param_defaults)[i], f.closure);
                    frame->define(f.params[i], std::move(dv));
                }
            }
            try {
                run_block(*f.body, frame);
            } catch (ReturnSignal& r) {
                return std::move(r.value);
            }
            return Value::nil();
        }
        throw Diag(e.callee->span,
            std::string("cannot call ") + callee.type_name());
    }

    // ---- Helpers ---------------------------------------------------------

    static bool truthy(const Value& v, Span s) {
        if (v.is_bool()) return v.as_bool();
        if (v.is_nil())  return false;
        if (v.is_num())  return v.as_num() != 0.0;
        throw Diag(s,
            std::string("condition must be bool/num/nil, got ") + v.type_name());
    }

    static int to_index(const Value& v, Span s) {
        if (!v.is_num())
            throw Diag(s, std::string("index must be a number, got ") + v.type_name());
        double d = v.as_num();
        if (d != std::floor(d))
            throw Diag(s, "index must be an integer, got " + std::to_string(d));
        return (int)d;
    }

    // Python-style: negative indices count from the end. Throws on out of range.
    static int wrap_index(int i, int size, Span s, const char* what) {
        int orig = i;
        if (i < 0) i += size;
        if (i < 0 || i >= size)
            throw Diag(s,
                std::string(what) + " index " + std::to_string(orig)
                + " out of range [-" + std::to_string(size)
                + ", " + std::to_string(size) + ")");
        return i;
    }

    // Tag check: if both `idx` and the container's axis have tags, they must
    // match (same container_id + axis).  Either side missing a tag = no
    // check (permissive).  Mismatch = a clear error.
    static void check_tag(const Value& idx, const ShapeTag* container_tag,
                          const char* axis_name, Span s) {
        if (!idx.tag) return;
        if (!container_tag) return;
        if (idx.tag->container_id == container_tag->container_id
         && idx.tag->axis == container_tag->axis) return;
        std::string msg = "index came from " + idx.tag->display
            + " but is being used as ";
        msg += axis_name;
        if (idx.tag->container_id != container_tag->container_id) {
            msg += " of a different container (expected ";
            msg += container_tag->display;
            msg += ")";
        } else {
            const char* got_axis =
                idx.tag->axis == 0 ? "row count"
                : idx.tag->axis == 1 ? "col count"
                : "length";
            msg += " (index carries ";
            msg += got_axis;
            msg += ")";
        }
        throw Diag(s, msg);
    }

    static bool values_equal(const Value& a, const Value& b) {
        if (a.v.index() != b.v.index()) return false;
        if (a.is_num())  return a.as_num()  == b.as_num();
        if (a.is_bool()) return a.as_bool() == b.as_bool();
        if (a.is_str())  return a.as_str()  == b.as_str();
        if (a.is_nil())  return true;
        return false; // vec/mat/fn: identity comparison via pointer would be misleading
    }

public:
    static std::string format_value(const Value& v) {
        std::ostringstream os;
        if (v.is_nil())  { os << "nil"; }
        else if (v.is_bool()) { os << (v.as_bool() ? "true" : "false"); }
        else if (v.is_num())  {
            double d = v.as_num();
            if (d == std::floor(d) && std::abs(d) < 1e16) os << (long long)d;
            else os << d;
        }
        else if (v.is_str()) { os << v.as_str(); }
        else if (v.is_vec()) {
            const Vec& x = v.as_vec();
            os << "[";
            for (size_t i = 0; i < x.size(); ++i) {
                if (i) os << ", ";
                os << x[i];
            }
            os << "]";
        }
        else if (v.is_mat()) {
            const Mat& m = v.as_mat();
            os << "[";
            for (size_t i = 0; i < m.rows; ++i) {
                if (i) os << ",\n ";
                os << "[";
                for (size_t j = 0; j < m.cols; ++j) {
                    if (j) os << ", ";
                    os << m.at(i, j);
                }
                os << "]";
            }
            os << "]";
        }
        else if (v.is_list()) {
            const ValueList& lst = v.as_list();
            os << "[";
            for (size_t i = 0; i < lst.size(); ++i) {
                if (i) os << ", ";
                os << format_value(lst[i]);
            }
            os << "]";
        }
        else if (v.is_fn())      { os << "<fn " << v.as_fn().name << ">"; }
        else if (v.is_builtin()) { os << "<builtin " << v.as_builtin().name << ">"; }
        return os.str();
    }

private:
    // ---- Builtins --------------------------------------------------------
    void register_builtins();
};

// ---- Builtin implementations ----------------------------------------------

namespace builtins {

inline Value b_at(const std::vector<Value>& args, Span s) {
    // Read an element without tag checking. Useful in generic library code
    // where the caller's promise is that lengths match but the interpreter
    // can't see why.
    if (args.size() < 2) throw Diag(s, "at(container, i[, j])");
    const Value& c = args[0];
    if (c.is_vec() && args.size() == 2) {
        const Vec& v = c.as_vec();
        int i = (int)args[1].as_num();
        if (i < 0) i += (int)v.size();
        if (i < 0 || (size_t)i >= v.size())
            throw Diag(s, "at: index out of range");
        return Value::num(v[i]);
    }
    if (c.is_list() && args.size() == 2) {
        const ValueList& lst = c.as_list();
        int i = (int)args[1].as_num();
        if (i < 0) i += (int)lst.size();
        if (i < 0 || (size_t)i >= lst.size())
            throw Diag(s, "at: index out of range");
        return lst[(size_t)i];
    }
    if (c.is_mat() && args.size() == 3) {
        const Mat& m = c.as_mat();
        int i = (int)args[1].as_num();
        int j = (int)args[2].as_num();
        if (i < 0) i += (int)m.rows;
        if (j < 0) j += (int)m.cols;
        if (i < 0 || (size_t)i >= m.rows || j < 0 || (size_t)j >= m.cols)
            throw Diag(s, "at: index out of range");
        return Value::num(m.at(i, j));
    }
    throw Diag(s, "at: expected (vec, i), (list, i), or (mat, i, j)");
}

inline Value b_set(const std::vector<Value>& args, Span s) {
    // Untagged write counterpart to `at`.
    if (args.size() < 3) throw Diag(s, "set(container, i, value) or set(M, i, j, value)");
    const Value& c = args[0];
    if (c.is_vec() && args.size() == 3) {
        auto& sp = std::get<std::shared_ptr<Vec>>(c.v);
        int i = (int)args[1].as_num();
        if (i < 0) i += (int)sp->size();
        if (i < 0 || (size_t)i >= sp->size())
            throw Diag(s, "set: index out of range");
        if (!args[2].is_num()) throw Diag(s, "set: value must be num");
        (*sp)[i] = args[2].as_num();
        return Value::nil();
    }
    if (c.is_mat() && args.size() == 4) {
        auto& sp = std::get<std::shared_ptr<Mat>>(c.v);
        int i = (int)args[1].as_num();
        int j = (int)args[2].as_num();
        if (i < 0) i += (int)sp->rows;
        if (j < 0) j += (int)sp->cols;
        if (i < 0 || (size_t)i >= sp->rows || j < 0 || (size_t)j >= sp->cols)
            throw Diag(s, "set: index out of range");
        if (!args[3].is_num()) throw Diag(s, "set: value must be num");
        sp->at(i, j) = args[3].as_num();
        return Value::nil();
    }
    throw Diag(s, "set: expected (vec, i, v) or (mat, i, j, v)");
}

inline Value b_append(const std::vector<Value>& args, Span s) {
    if (args.size() != 2) throw Diag(s, "append(list, value)");
    const Value& c = args[0];
    if (!c.is_list())
        throw Diag(s, std::string("append: expected list, got ") + c.type_name());
    auto& sp = std::get<std::shared_ptr<ValueList>>(c.v);
    sp->push_back(args[1]);
    return Value::nil();
}

inline Value b_input(const std::vector<Value>&, Span) {
    std::string line;
    if (!std::getline(std::cin, line)) return Value::nil();
    return Value::str(line);
}

// Builtin replacement for the removed `print` statement keyword. Accepts any
// number of args; prints them space-separated, then a newline. With zero
// args it just prints a newline (useful for blank lines).
inline Value b_print(const std::vector<Value>& args, Span) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) std::cout << ' ';
        std::cout << Interpreter::format_value(args[i]);
    }
    std::cout << "\n";
    return Value::nil();
}

inline Value b_num(const std::vector<Value>& args, Span s) {
    if (args.size() != 1) throw Diag(s, "num expects 1 arg");
    if (args[0].is_num()) return args[0];
    if (args[0].is_str()) {
        try { return Value::num(std::stod(args[0].as_str())); }
        catch (...) { throw Diag(s, "cannot parse '" + args[0].as_str() + "' as number"); }
    }
    throw Diag(s, std::string("num: cannot convert ") + args[0].type_name());
}

inline Value b_str(const std::vector<Value>& args, Span s) {
    if (args.size() != 1) throw Diag(s, "str expects 1 arg");
    return Value::str(Interpreter::format_value(args[0]));
}

inline Value b_len(const std::vector<Value>& args, Span s) {
    if (args.size() != 1) throw Diag(s, "len expects 1 arg");
    if (args[0].is_vec()) {
        const auto& sp = std::get<std::shared_ptr<Vec>>(args[0].v);
        Value v = Value::num((double)sp->size());
        if (sp->len_tag) v.tag = sp->len_tag;
        return v;
    }
    if (args[0].is_str()) return Value::num((double)args[0].as_str().size());
    if (args[0].is_mat()) {
        const auto& sp = std::get<std::shared_ptr<Mat>>(args[0].v);
        Value v = Value::num((double)sp->rows);
        if (sp->row_tag) v.tag = sp->row_tag;
        return v;
    }
    if (args[0].is_list()) return Value::num((double)args[0].as_list().size());
    throw Diag(s, std::string("len: not defined for ") + args[0].type_name());
}

inline Value b_rows(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_mat()) throw Diag(s, "rows expects a mat");
    const auto& sp = std::get<std::shared_ptr<Mat>>(args[0].v);
    Value v = Value::num((double)sp->rows);
    if (sp->row_tag) v.tag = sp->row_tag;
    return v;
}

inline Value b_cols(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_mat()) throw Diag(s, "cols expects a mat");
    const auto& sp = std::get<std::shared_ptr<Mat>>(args[0].v);
    Value v = Value::num((double)sp->cols);
    if (sp->col_tag) v.tag = sp->col_tag;
    return v;
}

inline Value b_zeros(const std::vector<Value>& args, Span s) {
    if (args.size() == 1 && args[0].is_num()) {
        Vec v((size_t)args[0].as_num());
        if (args[0].tag) v.len_tag = args[0].tag;
        return Value::vec(std::move(v));
    }
    if (args.size() == 2 && args[0].is_num() && args[1].is_num()) {
        Mat m((size_t)args[0].as_num(), (size_t)args[1].as_num());
        if (args[0].tag) m.row_tag = args[0].tag;
        if (args[1].tag) m.col_tag = args[1].tag;
        return Value::mat(std::move(m));
    }
    throw Diag(s, "zeros(n) or zeros(r, c)");
}

inline Value b_ones(const std::vector<Value>& args, Span s) {
    if (args.size() == 1 && args[0].is_num()) {
        Vec v((size_t)args[0].as_num(), 1.0);
        if (args[0].tag) v.len_tag = args[0].tag;
        return Value::vec(std::move(v));
    }
    if (args.size() == 2 && args[0].is_num() && args[1].is_num()) {
        Mat m((size_t)args[0].as_num(), (size_t)args[1].as_num(), 1.0);
        if (args[0].tag) m.row_tag = args[0].tag;
        if (args[1].tag) m.col_tag = args[1].tag;
        return Value::mat(std::move(m));
    }
    throw Diag(s, "ones(n) or ones(r, c)");
}

inline Value b_eye(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num()) throw Diag(s, "eye(n)");
    Mat m = mat_eye((size_t)args[0].as_num());
    if (args[0].tag) { m.row_tag = args[0].tag; m.col_tag = args[0].tag; }
    return Value::mat(std::move(m));
}

inline Value b_dot(const std::vector<Value>& args, Span s) {
    if (args.size() != 2 || !args[0].is_vec() || !args[1].is_vec())
        throw Diag(s, "dot expects two vecs");
    return Value::num(vec_dot(args[0].as_vec(), args[1].as_vec()));
}

inline Value b_norm(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_vec()) throw Diag(s, "norm expects a vec");
    return Value::num(vec_norm(args[0].as_vec()));
}

inline Value b_matmul(const std::vector<Value>& args, Span s) {
    if (args.size() != 2) throw Diag(s, "matmul expects 2 args");
    const Value& a = args[0];
    const Value& b = args[1];
    if (a.is_mat() && b.is_mat()) return Value::mat(mat_mul(a.as_mat(), b.as_mat()));
    if (a.is_mat() && b.is_vec()) return Value::vec(mat_vec(a.as_mat(), b.as_vec()));
    throw Diag(s, "matmul: mat*mat or mat*vec only");
}

inline Value b_transpose(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_mat()) throw Diag(s, "transpose expects a mat");
    return Value::mat(mat_transpose(args[0].as_mat()));
}

inline Value b_sqrt(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num()) throw Diag(s, "sqrt(num)");
    return Value::num(std::sqrt(args[0].as_num()));
}

inline Value b_abs(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num()) throw Diag(s, "abs(num)");
    return Value::num(std::abs(args[0].as_num()));
}

inline Value b_sin(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num()) throw Diag(s, "sin(num)");
    return Value::num(std::sin(args[0].as_num()));
}

inline Value b_cos(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num()) throw Diag(s, "cos(num)");
    return Value::num(std::cos(args[0].as_num()));
}

inline Value b_exp(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num()) throw Diag(s, "exp(num)");
    return Value::num(std::exp(args[0].as_num()));
}

inline Value b_log(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num()) throw Diag(s, "log(num)");
    return Value::num(std::log(args[0].as_num()));
}

// ---- Extensions backed by C++ stdlib -----------------------------------
// These map to the same names the transpiler emits, so the interpreter and
// transpiler agree.

inline Value b_sort_vec(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_vec())
        throw Diag(s, "sort_vec(vec): in-place ascending sort");
    Vec& v = const_cast<Vec&>(args[0].as_vec());
    std::sort(v.data.begin(), v.data.end());
    return Value::num(0.0);  // imperative; return ignored
}

// Global RNG mirrored on the interpreter side so behavior matches --exec.
inline std::mt19937_64& interp_rng() {
    static std::mt19937_64 r(1234567ULL);
    return r;
}
inline Value b_rng_seed(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_num())
        throw Diag(s, "rng_seed(num)");
    double seed = args[0].as_num();
    uint64_t sd;
    if (seed >= 0 && seed < 1.8e19) sd = (uint64_t)seed;
    else { std::memcpy(&sd, &seed, sizeof(sd)); }
    interp_rng().seed(sd);
    return Value::num(0.0);
}
inline Value b_rng_uniform(const std::vector<Value>& args, Span s) {
    if (!args.empty()) throw Diag(s, "rng_uniform() takes no args");
    static std::uniform_real_distribution<double> d(0.0, 1.0);
    return Value::num(d(interp_rng()));
}
inline Value b_rng_normal(const std::vector<Value>& args, Span s) {
    if (!args.empty()) throw Diag(s, "rng_normal() takes no args");
    static std::normal_distribution<double> d(0.0, 1.0);
    return Value::num(d(interp_rng()));
}

inline Value b_read_csv(const std::vector<Value>& args, Span s) {
    if (args.size() != 1 || !args[0].is_str())
        throw Diag(s, "read_csv(path: str) -> mat");
    const std::string& path = args[0].as_str();
    std::ifstream f(path);
    if (!f) throw Diag(s, "read_csv: cannot open '" + path + "'");

    std::vector<std::vector<double>> rows;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line[first] == '#') continue;
        for (char& c : line) {
            if (c == ',' || c == ';' || c == '\t') c = ' ';
        }
        std::istringstream ss(line);
        std::vector<double> cells;
        double x;
        while (ss >> x) cells.push_back(x);
        if (!ss.eof())
            throw Diag(s, "read_csv: parse error at " + path + ":"
                       + std::to_string(lineno));
        if (!cells.empty()) rows.push_back(std::move(cells));
    }
    if (rows.empty()) return Value::mat(Mat());

    size_t cols = rows[0].size();
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() != cols)
            throw Diag(s, "read_csv: inconsistent row widths in " + path);
    }
    Mat m(rows.size(), cols);
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = 0; j < cols; ++j)
            m.at(i, j) = rows[i][j];
    return Value::mat(std::move(m));
}

inline Value b_write_csv(const std::vector<Value>& args, Span s) {
    if (args.size() != 2 || !args[0].is_mat() || !args[1].is_str())
        throw Diag(s, "write_csv(M: mat, path: str)");
    const Mat& m = args[0].as_mat();
    const std::string& path = args[1].as_str();
    std::ofstream f(path);
    if (!f) throw Diag(s, "write_csv: cannot open '" + path + "'");
    for (size_t i = 0; i < m.rows; ++i) {
        for (size_t j = 0; j < m.cols; ++j) {
            if (j) f << ',';
            f << m.at(i, j);
        }
        f << '\n';
    }
    return Value::num(1.0);
}

} // namespace builtins

inline void Interpreter::register_builtins() {
    auto reg = [&](const std::string& name, BuiltinFn fn) {
        Builtin b{name, fn};
        globals->define(name, Value::builtin(b));
    };
    reg("input",     builtins::b_input);
    reg("print",     builtins::b_print);
    reg("at",        builtins::b_at);
    reg("set",       builtins::b_set);
    reg("append",    builtins::b_append);
    reg("num",       builtins::b_num);
    reg("str",       builtins::b_str);
    reg("len",       builtins::b_len);
    reg("rows",      builtins::b_rows);
    reg("cols",      builtins::b_cols);
    reg("zeros",     builtins::b_zeros);
    reg("ones",      builtins::b_ones);
    reg("eye",       builtins::b_eye);
    reg("dot",       builtins::b_dot);
    reg("norm",      builtins::b_norm);
    reg("matmul",    builtins::b_matmul);
    reg("transpose", builtins::b_transpose);
    reg("sqrt",      builtins::b_sqrt);
    reg("abs",       builtins::b_abs);
    reg("sin",       builtins::b_sin);
    reg("cos",       builtins::b_cos);
    reg("exp",       builtins::b_exp);
    reg("log",       builtins::b_log);
    // Extensions backed by C++ stdlib.
    reg("sort_vec",   builtins::b_sort_vec);
    reg("rng_seed",   builtins::b_rng_seed);
    reg("rng_uniform", builtins::b_rng_uniform);
    reg("rng_normal", builtins::b_rng_normal);
    reg("read_csv",   builtins::b_read_csv);
    reg("write_csv",  builtins::b_write_csv);
}

} // namespace knot
