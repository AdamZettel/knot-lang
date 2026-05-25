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
// Status: foundation only. This commit ships the binary writer and a
// stub codegen that emits a "main returns 42.0" module, just to prove
// the pipeline (knot binary -> .wasm file -> browser instantiate -> run)
// end to end. Real codegen for the knot AST comes in later commits.

#include "ast.hpp"
#include "diag.hpp"
#include "wasm_writer.hpp"

#include <vector>

namespace knot {

class WasmCodegen {
public:
    // For now: ignore the program and emit a fixed test module that
    // exports a function `main` of type () -> f64 returning 42.0. This
    // is the smallest non-trivial WASM module and the right thing to
    // get the browser pipeline shaken out before adding real codegen.
    std::vector<uint8_t> compile_test() {
        wasm::WasmModule m;

        // Type 0: () -> f64.
        uint32_t t = m.add_type(
            /*params=*/  {},
            /*results=*/ { wasm::type::F64 });

        // Function 0: returns 42.0.
        wasm::WasmFunc f;
        f.type_idx = t;
        // body: f64.const 42.0
        f.body.push_back(wasm::op::F64_CONST);
        wasm::WasmWriter w;
        w.f64_le(42.0);
        for (uint8_t b : w.bytes()) f.body.push_back(b);
        uint32_t fn_idx = m.add_function(std::move(f));

        m.add_export("main", wasm::export_kind::FUNC, fn_idx);
        return m.emit();
    }

    // Real compile entry point. Not implemented yet -- next phase
    // (numerical core: f64 arithmetic, if/while/for, scalar calls).
    [[noreturn]] std::vector<uint8_t> compile(const std::vector<StmtPtr>& /*program*/,
                                              const std::string& /*source_filename*/) {
        throw Diag(Span{},
            "wasm codegen: not yet implemented. The foundation (binary "
            "writer + stub) is in place; real AST -> WASM lowering is "
            "still upcoming.");
    }
};

} // namespace knot
