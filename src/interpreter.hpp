#pragma once
#include "annotations.hpp"
#include "ast.hpp"
#include "diag.hpp"
#include "hash.hpp"
#include "linalg.hpp"
#include "phrases.hpp"
#include "value.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>

namespace knot {

// We use exceptions for control flow on `return`, `break`, and `continue`.
// Cheap and clear for an interpreter this small; not idiomatic for
// production-grade VMs.
struct ReturnSignal {
    Value value;
};
struct BreakSignal { Span span; };
struct ContinueSignal { Span span; };

// Helper for NaN-trapping (Interpreter::trap_nan). Called from the
// scalar-arith hook in apply_binop and from each math builtin that can
// produce non-finite values. `what` is a short label for the message.
class Interpreter;
inline void trap_check_finite(double v, Span s, const char* what);

class Interpreter {
public:
    // NaN-trapping mode (--trap-nan). When enabled, any arithmetic op or
    // math builtin that produces NaN or Inf throws a Diag with the span
    // of the producing operation, rather than letting the bad value
    // silently propagate through the rest of the program. Static so the
    // standalone builtin functions can check it without a signature change.
    static bool trap_nan;

    Interpreter() {
        globals = std::make_shared<Env>();
        register_builtins();
    }

    void enable_trap_nan() { trap_nan = true; }

    // Recording mode (--record). When enabled, after each statement we
    // dump the global Env to `trace_out` so the run can be replayed or
    // inspected post-mortem. v1 captures globals only (function-local
    // scopes are not yet traced) and writes a human-readable text format.
    void enable_record(std::ostream& out) {
        record_mode = true;
        trace_out = &out;
    }

    void run(const std::vector<StmtPtr>& program) {
        // The single source of truth for stmt-block execution -- including
        // step-mode lookup, composite-key matching, and per-key suppression
        // -- lives in run_block(). The top-level program is just another
        // block.
        if (trace_out) {
            *trace_out << "# knot trace v1\n";
            *trace_out << "# events (one per line, in execution order):\n";
            *trace_out << "#   STEP <n> line=<L>:<C> [in=<fname>]  -- a statement just finished\n";
            *trace_out << "#     <name> = <value>                  -- one per global\n";
            *trace_out << "#     LOCAL <name> = <value>            -- one per local in current frame\n";
            *trace_out << "#   CALL <fname>(<args>) at <L>:<C>     -- entered a user-defined fn\n";
            *trace_out << "#   RET  <fname> -> <value>             -- returned from a user-defined fn\n";
        }
        try {
            run_block(program, globals);
        } catch (const BreakSignal& b) {
            throw Diag(b.span, "'break' is not inside a loop");
        } catch (const ContinueSignal& c) {
            throw Diag(c.span, "'continue' is not inside a loop");
        }
    }

    // --fuzz entry point. Two-pass execution:
    //   1. Run every top-level FnDecl statement. This makes the
    //      user's alternative implementations visible in globals.
    //   2. Look up `alt_name` (e.g. "qr_solve") in globals, prebind
    //      `slot_name` (e.g. "linsolve") to that value, and add
    //      slot_name to cli_prebinds_ so any later top-level
    //      assignment to it in the source is silently skipped.
    //   3. Run the remaining (non-FnDecl) top-level statements.
    //
    // Throws Diag if `alt_name` isn't defined in globals after
    // pass 1. Other Diag's from the program execution propagate.
    void run_with_fuzz(const std::vector<StmtPtr>& program,
                       const std::string& slot_name,
                       const std::string& alt_name) {
        // Pass 1: top-level FnDecls.
        for (const auto& s : program) {
            if (s->kind == StmtKind::FnDecl) exec(*s, globals);
        }
        // Look up the alternative.
        Value* alt = globals->find(alt_name);
        if (!alt) {
            throw Diag(program.empty() ? Span{} : program.front()->span,
                "fuzz: alternative `" + alt_name
                + "` was not defined in the program");
        }
        // Prebind. Any subsequent top-level `slot_name = ...` is
        // skipped (see the Assign case in exec()).
        globals->define(slot_name, *alt);
        cli_prebinds_.insert(slot_name);
        // Pass 2: everything else.
        try {
            for (const auto& s : program) {
                if (s->kind != StmtKind::FnDecl) exec(*s, globals);
            }
        } catch (const BreakSignal& b) {
            throw Diag(b.span, "'break' is not inside a loop");
        } catch (const ContinueSignal& c) {
            throw Diag(c.span, "'continue' is not inside a loop");
        }
    }

    // --test mode entry point. Two-pass:
    //   1. Execute non-test top-level statements once. This sets up def's,
    //      stdlib-derived globals, etc. TestDecl statements are no-ops here.
    //   2. For each top-level `test "name" { body }`, run body in a fresh
    //      child scope of globals so per-test bindings don't leak.
    //
    // Each test passes iff its body runs to completion without throwing a
    // Diag (from panic() / assert*) or any other uncaught signal. We print
    // a per-test PASS/FAIL line plus a summary line that the unit-test
    // runner shell script parses. Returns the failure count.
    int run_tests(const std::vector<StmtPtr>& program, const std::string& display_name) {
        try {
            run_block(program, globals);
        } catch (const BreakSignal& b) {
            throw Diag(b.span, "'break' is not inside a loop");
        } catch (const ContinueSignal& c) {
            throw Diag(c.span, "'continue' is not inside a loop");
        }

        int passed = 0;
        int failed = 0;
        for (const auto& stmt : program) {
            if (stmt->kind != StmtKind::TestDecl) continue;
            const std::string& name = stmt->name;
            auto sub = std::make_shared<Env>(globals);
            try {
                run_block(stmt->body, sub);
                std::cout << "  PASS  " << name << "\n";
                ++passed;
            } catch (const Diag& d) {
                std::cout << "  FAIL  " << name << "\n";
                std::cout << "        line " << d.span.line << ":" << d.span.col
                          << ": " << d.what() << "\n";
                ++failed;
            } catch (const ReturnSignal&) {
                // A bare `return` inside a test block: treat as pass.
                std::cout << "  PASS  " << name << "\n";
                ++passed;
            } catch (const BreakSignal&) {
                std::cout << "  FAIL  " << name << "\n";
                std::cout << "        'break' is not inside a loop\n";
                ++failed;
            } catch (const ContinueSignal&) {
                std::cout << "  FAIL  " << name << "\n";
                std::cout << "        'continue' is not inside a loop\n";
                ++failed;
            } catch (const std::exception& e) {
                std::cout << "  FAIL  " << name << "\n";
                std::cout << "        " << e.what() << "\n";
                ++failed;
            }
        }
        // Summary line. The run_unit_tests.sh script parses this format.
        std::cout << display_name << ": " << passed << " passed, "
                  << failed << " failed\n";
        return failed;
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
    bool record_mode = false;
    std::ostream* trace_out = nullptr;
    size_t snapshot_index = 0;
    // Names of currently-executing user-defined functions, in call order.
    // Used by snapshot() to label STEP blocks with `in=<fname>` and by
    // emit_call/emit_ret to push/pop. Empty means we're at top level.
    std::vector<std::string> fn_stack;
    // Keys that have already been shown this run. Used to suppress duplicate
    // annotation printing in tight loops.
    std::unordered_set<std::string> seen_annotations;
    // Names that the CLI has prebound (currently only by --fuzz). Top-level
    // `name = expr` assigns to these are silently skipped so the script's
    // own default value doesn't clobber the CLI's choice. Cleared on
    // each new Interpreter instance.
    std::unordered_set<std::string> cli_prebinds_;

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
            for (const auto& s : body) {
                exec(*s, env);
                if (record_mode) snapshot(*s, env);
            }
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
                for (int k = 0; k < hit.consumed; ++k) {
                    exec(*body[i + k], env);
                    if (record_mode) snapshot(*body[i + k], env);
                }
                i += hit.consumed;
            } else {
                exec(*body[i], env);
                if (record_mode) snapshot(*body[i], env);
                ++i;
            }
        }
    }

    // Emit a CALL event into the trace when a user-defined function is
    // entered. Builtins are not logged -- they're "leaves" of the call
    // graph and logging every print()/at()/sqrt() would drown the signal.
    void emit_call(const std::string& name, const std::vector<Value>& args, Span site) {
        fn_stack.push_back(name);
        if (!trace_out) return;
        auto& out = *trace_out;
        out << "CALL " << name << "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) out << ", ";
            out << format_value(args[i]);
        }
        out << ") at line=" << site.line << ":" << site.col << "\n";
    }

    // Emit a RET event when a user-defined function returns (whether via
    // explicit `return` or by falling off the end of the body, which
    // returns nil). RET is not emitted when the function exits via an
    // uncaught exception -- in that case the trace will show an unmatched
    // CALL, which is itself useful information ("crashed inside f").
    void emit_ret(const std::string& name, const Value& value) {
        if (!fn_stack.empty()) fn_stack.pop_back();
        if (!trace_out) return;
        *trace_out << "RET " << name << " -> " << format_value(value) << "\n";
    }

    // Write a snapshot of program state to the trace stream after a
    // statement has finished executing. Captures both globals and the
    // currently-active function-local scopes.
    //
    // Format (extending v1): each STEP block is
    //   STEP <n> line=<L>:<C> [in=<fname>]
    //     name = value           -- one per global
    //     LOCAL name = value     -- one per local in the current call frame
    //
    // The local walk starts from the env passed in (the statement's
    // current scope) and proceeds up the parent chain. The first
    // occurrence of each name wins (innermost shadows outer), and we
    // stop just before reaching `globals` so we don't double-print
    // globals as locals. Sorted-name iteration so traces are byte-stable
    // across runs with the same source.
    void snapshot(const Stmt& s, EnvPtr env) {
        if (!trace_out) return;
        auto& out = *trace_out;
        out << "STEP " << snapshot_index
            << " line=" << s.span.line << ":" << s.span.col;
        if (!fn_stack.empty()) out << " in=" << fn_stack.back();
        out << "\n";

        // Globals first.
        std::vector<std::string> names;
        names.reserve(globals->bindings().size());
        for (const auto& [name, val] : globals->bindings()) {
            if (val.is_fn() || val.is_builtin()) continue;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            const Value& val = globals->bindings().at(name);
            out << "  " << name << " = " << format_value(val) << "\n";
        }

        // Locals from the env chain, skipping globals. Walk innermost
        // outward; first occurrence of each name wins.
        if (env && env.get() != globals.get()) {
            std::unordered_map<std::string, Value> locals;
            EnvPtr e = env;
            while (e && e.get() != globals.get()) {
                for (const auto& [name, val] : e->bindings()) {
                    if (val.is_fn() || val.is_builtin()) continue;
                    if (locals.find(name) != locals.end()) continue; // shadowed
                    locals[name] = val;
                }
                e = e->parent;
            }
            std::vector<std::string> local_names;
            local_names.reserve(locals.size());
            for (const auto& [name, _] : locals) local_names.push_back(name);
            std::sort(local_names.begin(), local_names.end());
            for (const auto& name : local_names) {
                out << "  LOCAL " << name << " = "
                    << format_value(locals.at(name)) << "\n";
            }
        }

        ++snapshot_index;
    }

    // ---- Statements ------------------------------------------------------

    void exec(const Stmt& s, EnvPtr env) {
        switch (s.kind) {
            case StmtKind::Assign: {
                Value v = eval(*s.expr, env);
                if (s.target) {
                    assign_index(*s.target, std::move(v), env);
                } else {
                    // Top-level assignment to a CLI-prebound name is
                    // silently skipped -- the CLI's choice wins. This
                    // is what makes `--fuzz NAME=...` work when the
                    // script also defines its own default for NAME.
                    if (env.get() == globals.get()
                     && cli_prebinds_.count(s.name)) {
                        return;
                    }
                    // Per-value (num) tags live on call expressions and
                    // for-loop iterators; storing to a variable abstracts.
                    // `n = rows(A)` makes n an ordinary count, so the
                    // shape-tag system stays a useful guard at the points
                    // where axis-meaning is fresh (the headline catch
                    // `for i to rows(A) { for j to cols(A) { M[j, i] = ... }`)
                    // and gets out of the way once values pass through
                    // an alias. Vec/mat tags live inside the value and
                    // aren't touched here.
                    if (v.is_num()) v.tag.reset();
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
                    try { run_block(s.body, sub); }
                    catch (const BreakSignal&)    { return; }
                    catch (const ContinueSignal&) { /* next iter */ }
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
                        try { run_block(s.body, sub); }
                        catch (const BreakSignal&)    { return; }
                        catch (const ContinueSignal&) { continue; }
                    }
                } else if (over.is_vec()) {
                    const Vec& vv = over.as_vec();
                    for (size_t i = 0; i < vv.size(); ++i) {
                        auto sub = std::make_shared<Env>(env);
                        sub->define(s.name, Value::num(vv[i]));
                        try { run_block(s.body, sub); }
                        catch (const BreakSignal&)    { return; }
                        catch (const ContinueSignal&) { continue; }
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
                            try { run_block(s.body, sub); }
                            catch (const BreakSignal&)    { return; }
                            catch (const ContinueSignal&) { continue; }
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
                            try { run_block(s.body, sub); }
                            catch (const BreakSignal&)    { return; }
                            catch (const ContinueSignal&) { continue; }
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
                            try { run_block(s.body, sub); }
                            catch (const BreakSignal&)    { return; }
                            catch (const ContinueSignal&) { continue; }
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
            case StmtKind::Break:    throw BreakSignal{s.span};
            case StmtKind::Continue: throw ContinueSignal{s.span};
            case StmtKind::TestDecl: {
                // Under normal execution, `test "name" { ... }` is a no-op.
                // The body is only executed by run_tests() under --test mode.
                return;
            }
            case StmtKind::Show: {
                // `show EXPR1, EXPR2, ...` prints one line per arg in
                // the form "<source text>: <value>". Bare `show` with
                // no args prints a blank line.
                if (s.show_exprs.empty()) {
                    std::cout << "\n";
                    return;
                }
                for (size_t i = 0; i < s.show_exprs.size(); ++i) {
                    Value v = eval(*s.show_exprs[i], env);
                    const std::string& label =
                        (i < s.show_labels.size() ? s.show_labels[i] : "<expr>");
                    std::cout << label << ": " << format_value(v) << "\n";
                }
                return;
            }
            case StmtKind::Narrate: {
                // `narrate EXPR` evaluates EXPR (string-valued) and
                // prints "# <text>" when narration is enabled. If
                // EXPR isn't a string we format_value it so users
                // can drop arbitrary values into narration without
                // explicit str() wrapping.
                if (!narration_enabled()) return;
                Value v = eval(*s.expr, env);
                std::cout << "# "
                          << (v.is_str() ? v.as_str() : format_value(v))
                          << "\n";
                // Step-through: wait for the user to press Enter.
                // Prompt goes to stderr so piped output stays clean.
                if (narration_step_enabled()) {
                    std::cout.flush();
                    std::cerr << "  (press Enter to continue) ";
                    std::cerr.flush();
                    std::string line;
                    std::getline(std::cin, line);
                }
                return;
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
            case ExprKind::FnExpr: {
                // Build a Function value pointing at the expression
                // body. The expression is owned by the surrounding
                // AST, which outlives the interpreter -- raw pointer
                // is safe.
                auto f = std::make_shared<Function>();
                f->params = e.params;
                f->expr_body = e.lhs.get();
                f->closure = env;
                f->name = "<fn>";
                return Value::fn(f);
            }
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
            trap_check_finite(result, s, "arithmetic");
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
                    // Scalar broadcast: vec/mat +/- scalar in either order.
                    if (l.is_vec() && r.is_num()) return Value::vec(vec_add_scalar(l.as_vec(), r.as_num()));
                    if (l.is_num() && r.is_vec()) return Value::vec(vec_add_scalar(r.as_vec(), l.as_num()));
                    if (l.is_mat() && r.is_num()) return Value::mat(mat_add_scalar(l.as_mat(), r.as_num()));
                    if (l.is_num() && r.is_mat()) return Value::mat(mat_add_scalar(r.as_mat(), l.as_num()));
                    break;
                case BinOp::Sub:
                    if (l.is_num() && r.is_num()) return propagate_tag(l.as_num() - r.as_num());
                    if (l.is_vec() && r.is_vec()) return Value::vec(vec_sub(l.as_vec(), r.as_vec()));
                    if (l.is_mat() && r.is_mat()) return Value::mat(mat_sub(l.as_mat(), r.as_mat()));
                    if (l.is_vec() && r.is_num()) return Value::vec(vec_sub_scalar(l.as_vec(), r.as_num()));
                    if (l.is_num() && r.is_vec()) return Value::vec(scalar_sub_vec(l.as_num(), r.as_vec()));
                    if (l.is_mat() && r.is_num()) return Value::mat(mat_sub_scalar(l.as_mat(), r.as_num()));
                    if (l.is_num() && r.is_mat()) return Value::mat(scalar_sub_mat(l.as_num(), r.as_mat()));
                    break;
                case BinOp::Mul:
                    if (l.is_num() && r.is_num()) return propagate_tag(l.as_num() * r.as_num());
                    if (l.is_num() && r.is_vec()) return Value::vec(vec_scale(r.as_vec(), l.as_num()));
                    if (l.is_vec() && r.is_num()) return Value::vec(vec_scale(l.as_vec(), r.as_num()));
                    if (l.is_num() && r.is_mat()) return Value::mat(mat_scale(r.as_mat(), l.as_num()));
                    if (l.is_mat() && r.is_num()) return Value::mat(mat_scale(l.as_mat(), r.as_num()));
                    // Hadamard. The dot product / matmul lives on `@`,
                    // so `*` between same-shape vecs/mats is elementwise.
                    if (l.is_vec() && r.is_vec()) return Value::vec(vec_emul(l.as_vec(), r.as_vec()));
                    if (l.is_mat() && r.is_mat()) return Value::mat(mat_emul(l.as_mat(), r.as_mat()));
                    break;
                case BinOp::Div:
                    if (l.is_num() && r.is_num()) {
                        if (r.as_num() == 0.0) throw Diag(s, "division by zero");
                        return propagate_tag(l.as_num() / r.as_num());
                    }
                    if (l.is_vec() && r.is_num()) return Value::vec(vec_scale(l.as_vec(), 1.0 / r.as_num()));
                    if (l.is_mat() && r.is_num()) return Value::mat(mat_scale(l.as_mat(), 1.0 / r.as_num()));
                    // Elementwise vec / vec and mat / mat, plus the
                    // num / vec form so users can write `1 / x`.
                    if (l.is_vec() && r.is_vec()) return Value::vec(vec_ediv(l.as_vec(), r.as_vec()));
                    if (l.is_mat() && r.is_mat()) return Value::mat(mat_ediv(l.as_mat(), r.as_mat()));
                    if (l.is_num() && r.is_vec()) return Value::vec(scalar_div_vec(l.as_num(), r.as_vec()));
                    if (l.is_num() && r.is_mat()) return Value::mat(scalar_div_mat(l.as_num(), r.as_mat()));
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
            if (record_mode) emit_call(f.name, args, e.span);
            Value ret_value;
            // Expression-body functions (anonymous `fn(x) -> EXPR`)
            // evaluate the expression in the new frame and return
            // its value directly. Statement-body functions go
            // through run_block with the usual ReturnSignal flow.
            if (f.expr_body) {
                ret_value = eval(*f.expr_body, frame);
            } else {
                try {
                    run_block(*f.body, frame);
                    ret_value = Value::nil(); // implicit return
                } catch (ReturnSignal& r) {
                    ret_value = std::move(r.value);
                } catch (const BreakSignal& b) {
                    throw Diag(b.span, "'break' is not inside a loop");
                } catch (const ContinueSignal& c) {
                    throw Diag(c.span, "'continue' is not inside a loop");
                }
            }
            if (record_mode) emit_ret(f.name, ret_value);
            return ret_value;
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

// printf-style string formatting. `format("x = %.3f", x)` returns a string.
// We pass through to C's snprintf with the user's conversion spec, after
// type-checking the corresponding argument. Supported conversions:
//   %d %i %x %X %o %u %c  -- integer family (the num is cast to long long)
//   %f %e %E %g %G        -- float family
//   %s                    -- string
//   %%                    -- literal percent
// Flags, width, and precision in the spec are passed straight to snprintf.
inline Value b_format(const std::vector<Value>& args, Span s) {
    if (args.empty() || !args[0].is_str())
        throw Diag(s, "format(fmt_str, ...args): first arg must be a string");
    const std::string& fmt = args[0].as_str();
    std::string out;
    size_t arg_idx = 1;
    size_t i = 0;
    while (i < fmt.size()) {
        char c = fmt[i];
        if (c != '%') { out += c; ++i; continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out += '%'; i += 2; continue;
        }
        // Extract conversion spec: '%' [flags] [width] [.precision] conv
        size_t spec_start = i;
        ++i;
        while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) ++i;
        while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
        }
        if (i >= fmt.size())
            throw Diag(s, "format: incomplete conversion spec at end of format string");
        char conv = fmt[i++];
        std::string spec = fmt.substr(spec_start, i - spec_start);
        if (arg_idx >= args.size())
            throw Diag(s, "format: not enough arguments for format string");
        const Value& a = args[arg_idx++];
        // Print one value into a buffer, growing on truncation.
        auto try_once = [&](char* buf, size_t bufsize) -> int {
            if (conv=='d'||conv=='i'||conv=='x'||conv=='X'||conv=='o'||conv=='u'||conv=='c') {
                if (!a.is_num())
                    throw Diag(s, std::string("format: %") + conv + " expects num, got " + a.type_name());
                std::string llspec = spec.substr(0, spec.size() - 1) + "ll" + conv;
                return std::snprintf(buf, bufsize, llspec.c_str(), (long long)a.as_num());
            }
            if (conv=='f'||conv=='e'||conv=='E'||conv=='g'||conv=='G') {
                if (!a.is_num())
                    throw Diag(s, std::string("format: %") + conv + " expects num, got " + a.type_name());
                return std::snprintf(buf, bufsize, spec.c_str(), a.as_num());
            }
            if (conv == 's') {
                if (!a.is_str())
                    throw Diag(s, std::string("format: %s expects str, got ") + a.type_name());
                return std::snprintf(buf, bufsize, spec.c_str(), a.as_str().c_str());
            }
            throw Diag(s, std::string("format: unknown conversion '%") + conv + "'");
        };
        char small[128];
        int n = try_once(small, sizeof(small));
        if (n < 0) throw Diag(s, "format: snprintf failed");
        if ((size_t)n < sizeof(small)) {
            out.append(small, n);
        } else {
            std::string big((size_t)n + 1, '\0');
            try_once(&big[0], big.size());
            out.append(big.data(), (size_t)n);
        }
    }
    if (arg_idx < args.size())
        throw Diag(s, "format: more arguments than the format string consumed");
    return Value::str(out);
}

inline Value b_input(const std::vector<Value>&, Span) {
    std::string line;
    if (!std::getline(std::cin, line)) return Value::nil();
    return Value::str(line);
}

// Builtin replacement for the removed `print` statement keyword. Accepts any
// number of args; prints them space-separated, then a newline. With zero
// args it just prints a newline (useful for blank lines).
// panic(msg_str) -- abort the current test or program with `msg_str`.
// The thrown Diag's span is the call site, so failure messages point at
// the user's source. The assert*() helpers in stdlib.knot all go through
// this so they share a single failure path.
inline Value b_panic(const std::vector<Value>& args, Span s) {
    std::string msg = "panic";
    if (args.size() == 1) {
        if (args[0].is_str()) msg = args[0].as_str();
        else                  msg = std::string("panic: expected string, got ") + args[0].type_name();
    } else if (args.size() > 1) {
        throw Diag(s, "panic(msg): expected 0 or 1 string args, got " + std::to_string(args.size()));
    }
    throw Diag(s, msg);
}

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

// Format a double as JSON. NaN/Inf become `null` (JSON has no IEEE
// non-finites). Uses precision 17 so doubles round-trip exactly.
inline std::string json_double(double d) {
    if (std::isnan(d) || std::isinf(d)) return "null";
    std::ostringstream os;
    os.precision(17);
    os << d;
    return os.str();
}

// Minimal JSON string escape for the plot builtins: \ and " only.
// Knot strings are UTF-8 and we let everything else pass through.
inline std::string json_str(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (char c : in) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else                        { out.push_back(c); }
    }
    out.push_back('"');
    return out;
}

// Serialize a single trace (x, y, name) to its JSON object form:
//   {"name": "...", "x": [...], "y": [...], "kind": "line"}
// Shared by plot() (stdout marker) and plot_save() (file output).
inline std::string trace_json(const Vec& x, const Vec& y, const std::string& name) {
    std::ostringstream os;
    os << "{\"name\":" << json_str(name)
       << ",\"kind\":\"line\""
       << ",\"x\":[";
    for (size_t i = 0; i < x.size(); ++i) {
        if (i) os << ",";
        os << json_double(x[i]);
    }
    os << "],\"y\":[";
    for (size_t i = 0; i < y.size(); ++i) {
        if (i) os << ",";
        os << json_double(y[i]);
    }
    os << "]}";
    return os.str();
}

// plot(x, y)         -- single trace from two equal-length vecs
// plot(x, y, "name") -- as above with a trace label
//
// Emits a sentinel-prefixed JSON line to stdout. The browser playground
// (web/index.html) intercepts every line starting with __knot_plot__,
// pulls the JSON off the rest, and renders it via Plotly. Under the CLI
// the line passes through as-is. For CLI workflows that want to render
// later, use plot_save() instead -- it writes the same JSON to a file.
inline Value b_plot(const std::vector<Value>& args, Span s) {
    if (args.size() < 2 || args.size() > 3)
        throw Diag(s, "plot(x, y) or plot(x, y, name): expected 2 or 3 args");
    if (!args[0].is_vec() || !args[1].is_vec())
        throw Diag(s, "plot: x and y must both be vecs");
    const Vec& x = args[0].as_vec();
    const Vec& y = args[1].as_vec();
    if (x.size() != y.size())
        throw Diag(s, "plot: x and y must have the same length");
    std::string name = "trace";
    if (args.size() == 3) {
        if (!args[2].is_str())
            throw Diag(s, "plot: third arg (name) must be a string");
        name = args[2].as_str();
    }
    std::cout << "__knot_plot__ " << trace_json(x, y, name) << "\n";
    return Value::nil();
}

// plot_save(path, x, y)         -- one-trace JSON file
// plot_save(path, x, y, "name") -- with a trace name
// plot_save(path, x, Y)         -- mat Y, one trace per column (named "col_0", "col_1", ...)
// plot_save(path, x, Y, "name") -- as above, trace names are "name 0", "name 1", ...
//
// Writes a JSON file the standalone viewer (web/viewer.html) can load.
// JSON shape: { "traces": [ {"name":..., "kind":"line", "x":[...], "y":[...]}, ... ] }
//
// Each call OVERWRITES the file with one fresh document. For multi-
// trace plots, pass a mat or build the data into a single call;
// appending across multiple calls is intentionally not supported
// (state in a file across calls is a hard-to-debug source of bugs).
inline Value b_plot_save(const std::vector<Value>& args, Span s) {
    if (args.size() < 3 || args.size() > 4)
        throw Diag(s, "plot_save(path, x, y[, name]) or plot_save(path, x, Y[, name])");
    if (!args[0].is_str())
        throw Diag(s, "plot_save: first arg must be a string path");
    if (!args[1].is_vec())
        throw Diag(s, "plot_save: second arg must be a vec (x values)");
    const std::string& path = args[0].as_str();
    const Vec& x = args[1].as_vec();
    std::string base_name = "trace";
    if (args.size() == 4) {
        if (!args[3].is_str())
            throw Diag(s, "plot_save: name arg must be a string");
        base_name = args[3].as_str();
    }

    std::ostringstream out;
    out << "{\"traces\":[";

    if (args[2].is_vec()) {
        const Vec& y = args[2].as_vec();
        if (x.size() != y.size())
            throw Diag(s, "plot_save: x and y must have the same length");
        out << trace_json(x, y, base_name);
    } else if (args[2].is_mat()) {
        const Mat& Y = args[2].as_mat();
        if (Y.rows != x.size())
            throw Diag(s, "plot_save: matrix row count must match x length");
        for (size_t j = 0; j < Y.cols; ++j) {
            Vec col((size_t)Y.rows);
            for (size_t i = 0; i < Y.rows; ++i) col[i] = Y.at(i, j);
            std::string nm = (args.size() == 4)
                ? base_name + " " + std::to_string(j)
                : "col_" + std::to_string(j);
            if (j) out << ",";
            out << trace_json(x, col, nm);
        }
    } else {
        throw Diag(s, "plot_save: third arg must be a vec or a mat");
    }
    out << "]}";

    std::ofstream f(path);
    if (!f) throw Diag(s, "plot_save: cannot open '" + path + "' for writing");
    f << out.str();
    return Value::nil();
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

// Apply a scalar function elementwise across num / vec / mat. The
// shape of the input determines the shape of the output; len_tag /
// row_tag / col_tag are preserved so broadcast results still play
// nicely with downstream shape-checking. `trap` decides whether the
// per-element result goes through trap_check_finite (so under
// --trap-nan, broadcasting sqrt over a vec containing -1 fires at the
// builtin call site, with the same wording as the scalar form).
template <typename F>
inline Value apply_unary_broadcast(const std::vector<Value>& args,
                                   Span s,
                                   const char* name,
                                   F f,
                                   bool trap) {
    if (args.size() != 1)
        throw Diag(s, std::string(name) + "(num | vec | mat)");
    const Value& a = args[0];

    if (a.is_num()) {
        double r = f(a.as_num());
        if (trap) trap_check_finite(r, s, name);
        return Value::num(r);
    }

    if (a.is_vec()) {
        const Vec& v = a.as_vec();
        Vec out(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            double r = f(v[i]);
            if (trap) trap_check_finite(r, s, name);
            out[i] = r;
        }
        out.len_tag = v.len_tag;
        return Value::vec(std::move(out));
    }

    if (a.is_mat()) {
        const Mat& m = a.as_mat();
        Mat out(m.rows, m.cols);
        for (size_t i = 0; i < m.rows; ++i) {
            for (size_t j = 0; j < m.cols; ++j) {
                double r = f(m.at(i, j));
                if (trap) trap_check_finite(r, s, name);
                out.at(i, j) = r;
            }
        }
        out.row_tag = m.row_tag;
        out.col_tag = m.col_tag;
        return Value::mat(std::move(out));
    }

    throw Diag(s, std::string(name) + ": expected num, vec, or mat, got "
                  + a.type_name());
}

inline Value b_sqrt(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "sqrt",
        [](double x){ return std::sqrt(x); }, /*trap=*/true);
}

inline Value b_abs(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "abs",
        [](double x){ return std::abs(x); }, /*trap=*/false);
}

inline Value b_sin(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "sin",
        [](double x){ return std::sin(x); }, /*trap=*/false);
}

inline Value b_cos(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "cos",
        [](double x){ return std::cos(x); }, /*trap=*/false);
}

inline Value b_exp(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "exp",
        [](double x){ return std::exp(x); }, /*trap=*/true);
}

inline Value b_log(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "log",
        [](double x){ return std::log(x); }, /*trap=*/true);
}

inline Value b_tan(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "tan",
        [](double x){ return std::tan(x); }, /*trap=*/false);
}

inline Value b_asin(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "asin",
        [](double x){ return std::asin(x); }, /*trap=*/true);
}

inline Value b_acos(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "acos",
        [](double x){ return std::acos(x); }, /*trap=*/true);
}

inline Value b_atan(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "atan",
        [](double x){ return std::atan(x); }, /*trap=*/false);
}

inline Value b_floor(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "floor",
        [](double x){ return std::floor(x); }, /*trap=*/false);
}

inline Value b_ceil(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "ceil",
        [](double x){ return std::ceil(x); }, /*trap=*/false);
}

inline Value b_round(const std::vector<Value>& args, Span s) {
    return apply_unary_broadcast(args, s, "round",
        [](double x){ return std::round(x); }, /*trap=*/false);
}

// Two-argument atan(y, x) -- the quadrant-aware inverse tangent.
// Num-only for v1; broadcasting over equal-length vecs is a natural
// extension later.
inline Value b_atan2(const std::vector<Value>& args, Span s) {
    if (args.size() != 2 || !args[0].is_num() || !args[1].is_num())
        throw Diag(s, "atan2(y, x): expected two nums");
    return Value::num(std::atan2(args[0].as_num(), args[1].as_num()));
}

// pow(x, n) -- knot has no `**` operator. Accepts num base + num
// exponent. Real exponents are allowed (so sqrt(x) == pow(x, 0.5)).
inline Value b_pow(const std::vector<Value>& args, Span s) {
    if (args.size() != 2 || !args[0].is_num() || !args[1].is_num())
        throw Diag(s, "pow(x, n): expected two nums");
    double r = std::pow(args[0].as_num(), args[1].as_num());
    trap_check_finite(r, s, "pow");
    return Value::num(r);
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

// Out-of-line definitions. The Interpreter::trap_nan flag is a single
// process-wide switch flipped by enable_trap_nan(); checking it from the
// standalone builtin functions just needs the class name, no instance.
inline bool Interpreter::trap_nan = false;

inline void trap_check_finite(double v, Span s, const char* what) {
    if (Interpreter::trap_nan && !std::isfinite(v)) {
        std::string msg = std::string("--trap-nan: ") + what + " produced "
            + (std::isnan(v) ? "NaN" : (v > 0 ? "+Inf" : "-Inf"))
            + " (run without --trap-nan to allow non-finite values)";
        throw Diag(s, msg);
    }
}

inline void Interpreter::register_builtins() {
    auto reg = [&](const std::string& name, BuiltinFn fn) {
        Builtin b{name, fn};
        globals->define(name, Value::builtin(b));
    };
    reg("input",     builtins::b_input);
    reg("print",     builtins::b_print);
    reg("panic",     builtins::b_panic);
    reg("plot",      builtins::b_plot);
    reg("plot_save", builtins::b_plot_save);
    reg("at",        builtins::b_at);
    reg("set",       builtins::b_set);
    reg("append",    builtins::b_append);
    reg("format",    builtins::b_format);
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
    reg("tan",       builtins::b_tan);
    reg("asin",      builtins::b_asin);
    reg("acos",      builtins::b_acos);
    reg("atan",      builtins::b_atan);
    reg("atan2",     builtins::b_atan2);
    reg("exp",       builtins::b_exp);
    reg("log",       builtins::b_log);
    reg("pow",       builtins::b_pow);
    reg("floor",     builtins::b_floor);
    reg("ceil",      builtins::b_ceil);
    reg("round",     builtins::b_round);
    // Extensions backed by C++ stdlib.
    reg("sort_vec",   builtins::b_sort_vec);
    reg("rng_seed",   builtins::b_rng_seed);
    reg("rng_uniform", builtins::b_rng_uniform);
    reg("rng_normal", builtins::b_rng_normal);
    reg("read_csv",   builtins::b_read_csv);
    reg("write_csv",  builtins::b_write_csv);
}

} // namespace knot
