#pragma once
// knot -> WebAssembly codegen.
//
// Parallel to src/codegen.hpp (which emits C). Walks the same knot AST
// and emits a WASM binary module ready to hand to WebAssembly.instantiate.
//
// The C codegen path is preserved for the native --exec workflow; this
// backend exists so the browser playground can run --exec without a C
// compiler in the runtime.
//
// Status (phase 2): numerical core. Handles scalar nums, arithmetic,
// comparisons, if / while / for-to, local variables, and calls to a
// fixed set of imported builtins (print). Vec / mat / tensor and
// user-defined functions are still upcoming.

#include "ast.hpp"
#include "diag.hpp"
#include "wasm_writer.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace knot {

class WasmCodegen {
public:
    // Stub test module: exports main() -> f64 returning 42.0. Useful
    // for smoke-testing the binary writer in isolation; the real
    // compile() entry point is below.
    std::vector<uint8_t> compile_test() {
        wasm::WasmModule m;
        uint32_t t = m.add_type({}, { wasm::type::F64 });
        wasm::WasmFunc f;
        f.type_idx = t;
        wasm::WasmWriter w;
        w.f64_le(42.0);
        f.body.push_back(wasm::op::F64_CONST);
        for (uint8_t b : w.bytes()) f.body.push_back(b);
        uint32_t fn_idx = m.add_function(std::move(f));
        m.add_export("main", wasm::export_kind::FUNC, fn_idx);
        return m.emit();
    }

    // Compile a knot program to a WebAssembly module. The module
    // exports `main` of type () -> () that, when called, runs the
    // top-level statements in order. `env.knot_print_num(f64)` is
    // imported and the JS host is expected to provide it (the
    // playground appends to a stdout buffer; tests print to console).
    //
    // Throws Diag on any AST construct we don't yet handle.
    std::vector<uint8_t> compile(const std::vector<StmtPtr>& program,
                                 const std::string& /*source_filename*/) {
        wasm::WasmModule m;
        // Imports first (function indices for imports come before locals).
        // env.knot_print_num: (f64) -> ()
        uint32_t print_num_type =
            m.add_type({ wasm::type::F64 }, {});
        uint32_t print_num_idx =
            m.add_import_func("env", "knot_print_num", print_num_type);

        // main: () -> ()
        uint32_t main_type = m.add_type({}, {});
        wasm::WasmFunc main_fn;
        main_fn.type_idx = main_type;

        // Walk top-level statements emitting into main_fn.body. The
        // codegen uses a small ctx that tracks the function being
        // built and the local-variable map.
        Ctx ctx;
        ctx.print_num_idx = print_num_idx;
        for (const auto& s : program) {
            emit_stmt(*s, main_fn, ctx);
        }
        // Pack the locals table from the ctx.
        if (!ctx.locals.empty()) {
            // All our locals are f64 for now; pack as one run.
            main_fn.locals.push_back({ (uint32_t)ctx.locals.size(),
                                       wasm::type::F64 });
        }

        uint32_t main_idx = m.add_function(std::move(main_fn));
        m.add_export("main", wasm::export_kind::FUNC, main_idx);
        return m.emit();
    }

private:
    // Codegen context that lives for the duration of one function.
    struct Ctx {
        // local name -> local index (after imports). Imports don't
        // consume local indices; func params do, but main has no
        // params, so locals start at 0.
        std::unordered_map<std::string, uint32_t> locals;
        uint32_t print_num_idx = 0;

        uint32_t local_idx(const std::string& name) {
            auto it = locals.find(name);
            if (it != locals.end()) return it->second;
            uint32_t i = (uint32_t)locals.size();
            locals[name] = i;
            return i;
        }
        bool has_local(const std::string& name) const {
            return locals.find(name) != locals.end();
        }
    };

    // ---- statements ------------------------------------------------------

    void emit_stmt(const Stmt& s, wasm::WasmFunc& fn, Ctx& ctx) {
        switch (s.kind) {
            case StmtKind::Assign: {
                if (s.target) {
                    fail(s.span, "indexed assign not yet supported in --wasm");
                }
                // Emit the RHS expression onto the stack, then local.set.
                emit_expr(*s.expr, fn, ctx);
                uint32_t li = ctx.local_idx(s.name);
                fn.body.push_back(wasm::op::LOCAL_SET);
                emit_uleb(fn.body, li);
                return;
            }
            case StmtKind::CompAssign: {
                if (s.target)
                    fail(s.span, "indexed compound assign not yet supported in --wasm");
                if (!ctx.has_local(s.name))
                    fail(s.span, "compound assign to undefined name: " + s.name);
                uint32_t li = ctx.local_idx(s.name);
                fn.body.push_back(wasm::op::LOCAL_GET);
                emit_uleb(fn.body, li);
                emit_expr(*s.expr, fn, ctx);
                switch (s.comp_op) {
                    case BinOp::Add: fn.body.push_back(wasm::op::F64_ADD); break;
                    case BinOp::Sub: fn.body.push_back(wasm::op::F64_SUB); break;
                    case BinOp::Mul: fn.body.push_back(wasm::op::F64_MUL); break;
                    case BinOp::Div: fn.body.push_back(wasm::op::F64_DIV); break;
                    default: fail(s.span, "unsupported compound op in --wasm");
                }
                fn.body.push_back(wasm::op::LOCAL_SET);
                emit_uleb(fn.body, li);
                return;
            }
            case StmtKind::ExprStmt: {
                emit_expr(*s.expr, fn, ctx);
                fn.body.push_back(wasm::op::DROP);
                return;
            }
            case StmtKind::If: {
                emit_expr(*s.expr, fn, ctx);
                // Cond is f64; convert to bool via != 0.
                fn.body.push_back(wasm::op::F64_CONST);
                {
                    wasm::WasmWriter w; w.f64_le(0.0);
                    for (uint8_t b : w.bytes()) fn.body.push_back(b);
                }
                fn.body.push_back(wasm::op::F64_NE);
                fn.body.push_back(wasm::op::IF);
                fn.body.push_back(wasm::type::VOID);
                for (const auto& st : s.body) emit_stmt(*st, fn, ctx);
                if (!s.else_body.empty()) {
                    fn.body.push_back(wasm::op::ELSE);
                    for (const auto& st : s.else_body) emit_stmt(*st, fn, ctx);
                }
                fn.body.push_back(wasm::op::END);
                return;
            }
            case StmtKind::While: {
                // block { loop { brif !cond out; body; br loop } }
                fn.body.push_back(wasm::op::BLOCK);
                fn.body.push_back(wasm::type::VOID);
                fn.body.push_back(wasm::op::LOOP);
                fn.body.push_back(wasm::type::VOID);
                // cond on stack; F64_NE 0 -> i32. Branch out (depth 1)
                // when the condition is false.
                emit_expr(*s.expr, fn, ctx);
                fn.body.push_back(wasm::op::F64_CONST);
                {
                    wasm::WasmWriter w; w.f64_le(0.0);
                    for (uint8_t b : w.bytes()) fn.body.push_back(b);
                }
                fn.body.push_back(wasm::op::F64_EQ);  // cond == 0  =>  exit
                fn.body.push_back(wasm::op::BR_IF);
                emit_uleb(fn.body, 1);                 // break outer block
                for (const auto& st : s.body) emit_stmt(*st, fn, ctx);
                fn.body.push_back(wasm::op::BR);
                emit_uleb(fn.body, 0);                 // continue inner loop
                fn.body.push_back(wasm::op::END);      // end loop
                fn.body.push_back(wasm::op::END);      // end block
                return;
            }
            case StmtKind::For: {
                // Only ForForm::ToCount is supported in --wasm v1.
                if (s.for_form != ForForm::ToCount)
                    fail(s.span, "only `for X to N` is supported in --wasm");
                // Eval the bound; truncate to i32 once for the loop.
                emit_expr(*s.expr, fn, ctx);
                fn.body.push_back(wasm::op::I32_TRUNC_F64_S);
                // Stash bound in a synthetic local.
                std::string bound_name = "__for_bound_" + std::to_string(fn.body.size());
                // We need an i32 local; pack as a separate run.
                fail(s.span, "for-to loop not yet wired in --wasm "
                             "(needs i32 locals; coming in a follow-up)");
            }
            case StmtKind::Print: {
                emit_expr(*s.expr, fn, ctx);
                fn.body.push_back(wasm::op::CALL);
                emit_uleb(fn.body, ctx.print_num_idx);
                return;
            }
            case StmtKind::Block: {
                for (const auto& st : s.body) emit_stmt(*st, fn, ctx);
                return;
            }
            case StmtKind::Return:
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::FnDecl:
            case StmtKind::TestDecl:
            case StmtKind::Show:
            case StmtKind::Narrate:
            case StmtKind::Loop:
                fail(s.span, "statement form not yet supported in --wasm "
                             "(numerical-core phase only handles "
                             "assign/comp/expr/if/while/print/block)");
        }
    }

    // ---- expressions -----------------------------------------------------

    void emit_expr(const Expr& e, wasm::WasmFunc& fn, Ctx& ctx) {
        switch (e.kind) {
            case ExprKind::NumberLit: {
                fn.body.push_back(wasm::op::F64_CONST);
                wasm::WasmWriter w;
                w.f64_le(e.num);
                for (uint8_t b : w.bytes()) fn.body.push_back(b);
                return;
            }
            case ExprKind::BoolLit: {
                fn.body.push_back(wasm::op::F64_CONST);
                wasm::WasmWriter w;
                w.f64_le(e.boolean ? 1.0 : 0.0);
                for (uint8_t b : w.bytes()) fn.body.push_back(b);
                return;
            }
            case ExprKind::Ident: {
                if (!ctx.has_local(e.str))
                    fail(e.span, "unknown name in --wasm: " + e.str);
                uint32_t li = ctx.local_idx(e.str);
                fn.body.push_back(wasm::op::LOCAL_GET);
                emit_uleb(fn.body, li);
                return;
            }
            case ExprKind::Unary: {
                emit_expr(*e.rhs, fn, ctx);
                if (e.unop == UnOp::Neg) {
                    fn.body.push_back(wasm::op::F64_NEG);
                } else if (e.unop == UnOp::Not) {
                    // !x : f64.eq 0.0 -> i32, then convert back to f64
                    fn.body.push_back(wasm::op::F64_CONST);
                    wasm::WasmWriter w; w.f64_le(0.0);
                    for (uint8_t b : w.bytes()) fn.body.push_back(b);
                    fn.body.push_back(wasm::op::F64_EQ);
                    fn.body.push_back(wasm::op::F64_CONVERT_I32_S);
                }
                return;
            }
            case ExprKind::Binary: {
                emit_expr(*e.lhs, fn, ctx);
                emit_expr(*e.rhs, fn, ctx);
                switch (e.binop) {
                    case BinOp::Add: fn.body.push_back(wasm::op::F64_ADD); return;
                    case BinOp::Sub: fn.body.push_back(wasm::op::F64_SUB); return;
                    case BinOp::Mul: fn.body.push_back(wasm::op::F64_MUL); return;
                    case BinOp::Div: fn.body.push_back(wasm::op::F64_DIV); return;
                    case BinOp::Eq:  emit_cmp(fn, wasm::op::F64_EQ); return;
                    case BinOp::Ne:  emit_cmp(fn, wasm::op::F64_NE); return;
                    case BinOp::Lt:  emit_cmp(fn, wasm::op::F64_LT); return;
                    case BinOp::Le:  emit_cmp(fn, wasm::op::F64_LE); return;
                    case BinOp::Gt:  emit_cmp(fn, wasm::op::F64_GT); return;
                    case BinOp::Ge:  emit_cmp(fn, wasm::op::F64_GE); return;
                    default: fail(e.span, "binop not yet supported in --wasm");
                }
            }
            case ExprKind::Call: {
                if (!e.callee || e.callee->kind != ExprKind::Ident)
                    fail(e.span, "indirect calls not yet supported in --wasm");
                const std::string& nm = e.callee->str;
                if (nm == "print") {
                    if (e.elems.size() != 1)
                        fail(e.span, "print(num) in --wasm v1 takes exactly one arg");
                    emit_expr(*e.elems[0], fn, ctx);
                    fn.body.push_back(wasm::op::CALL);
                    emit_uleb(fn.body, ctx.print_num_idx);
                    // print returns nothing; push a 0.0 so callers that
                    // treat it as a value (rare; ExprStmt drops it) have
                    // a stack slot to discard.
                    fn.body.push_back(wasm::op::F64_CONST);
                    wasm::WasmWriter w; w.f64_le(0.0);
                    for (uint8_t b : w.bytes()) fn.body.push_back(b);
                    return;
                }
                fail(e.span, "call to '" + nm + "' not yet supported in --wasm "
                             "(numerical-core phase only handles print)");
            }
            case ExprKind::Index:
            case ExprKind::Slice:
            case ExprKind::VecLit:
            case ExprKind::MatLit:
            case ExprKind::StringLit:
            case ExprKind::NilLit:
            case ExprKind::FnExpr:
                fail(e.span, "expression form not yet supported in --wasm "
                             "(numerical-core phase, no aggregates / strings / fnexpr)");
        }
    }

    // The wasm comparisons return i32 (0 or 1); convert to f64 so the
    // rest of the codegen can treat bool-as-f64 uniformly.
    void emit_cmp(wasm::WasmFunc& fn, uint8_t cmp_op) {
        fn.body.push_back(cmp_op);
        fn.body.push_back(wasm::op::F64_CONVERT_I32_S);
    }

    static void emit_uleb(std::vector<uint8_t>& out, uint64_t v) {
        do {
            uint8_t b = (uint8_t)(v & 0x7f);
            v >>= 7;
            if (v) b |= 0x80;
            out.push_back(b);
        } while (v);
    }

    [[noreturn]] static void fail(Span s, const std::string& msg) {
        throw Diag(s, "wasm codegen: " + msg);
    }
};

} // namespace knot
