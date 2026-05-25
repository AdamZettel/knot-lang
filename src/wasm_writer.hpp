#pragma once
// Low-level WebAssembly binary writer.
//
// A small, focused helper for emitting WASM modules from the knot
// codegen pass. No external dependencies; the WASM binary format is
// stable and well-documented (https://webassembly.github.io/spec/),
// and a hand-rolled writer is the right amount of code -- a few
// hundred lines, much smaller than pulling in binaryen or wabt.
//
// The shape:
//   WasmWriter is a byte buffer with LEB128 / f64 / string helpers.
//   WasmModule sits on top and tracks the various indexed tables
//   (types, functions, imports, exports, etc.) so callers don't have
//   to compute sizes by hand. .emit() produces the final byte vector.
//
// This file is the binary serialization layer only; knot AST -> WASM
// (codegen) lives separately in src/wasm_codegen.hpp.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace knot {
namespace wasm {

// ---- Opcodes (subset; we'll grow this as the codegen needs more) ---------

namespace op {
    constexpr uint8_t UNREACHABLE = 0x00;
    constexpr uint8_t NOP         = 0x01;
    constexpr uint8_t BLOCK       = 0x02;
    constexpr uint8_t LOOP        = 0x03;
    constexpr uint8_t IF          = 0x04;
    constexpr uint8_t ELSE        = 0x05;
    constexpr uint8_t END         = 0x0b;
    constexpr uint8_t BR          = 0x0c;
    constexpr uint8_t BR_IF       = 0x0d;
    constexpr uint8_t RETURN      = 0x0f;
    constexpr uint8_t CALL        = 0x10;
    constexpr uint8_t CALL_INDIRECT = 0x11;
    constexpr uint8_t DROP        = 0x1a;
    constexpr uint8_t LOCAL_GET   = 0x20;
    constexpr uint8_t LOCAL_SET   = 0x21;
    constexpr uint8_t LOCAL_TEE   = 0x22;
    constexpr uint8_t GLOBAL_GET  = 0x23;
    constexpr uint8_t GLOBAL_SET  = 0x24;
    constexpr uint8_t I32_LOAD    = 0x28;
    constexpr uint8_t F64_LOAD    = 0x2b;
    constexpr uint8_t I32_STORE   = 0x36;
    constexpr uint8_t F64_STORE   = 0x39;
    constexpr uint8_t I32_CONST   = 0x41;
    constexpr uint8_t I64_CONST   = 0x42;
    constexpr uint8_t F32_CONST   = 0x43;
    constexpr uint8_t F64_CONST   = 0x44;

    // i32 comparisons
    constexpr uint8_t I32_EQZ     = 0x45;
    constexpr uint8_t I32_EQ      = 0x46;
    constexpr uint8_t I32_NE      = 0x47;
    constexpr uint8_t I32_LT_S    = 0x48;
    constexpr uint8_t I32_GT_S    = 0x4a;
    constexpr uint8_t I32_LE_S    = 0x4c;
    constexpr uint8_t I32_GE_S    = 0x4e;

    // f64 comparisons
    constexpr uint8_t F64_EQ      = 0x61;
    constexpr uint8_t F64_NE      = 0x62;
    constexpr uint8_t F64_LT      = 0x63;
    constexpr uint8_t F64_GT      = 0x64;
    constexpr uint8_t F64_LE      = 0x65;
    constexpr uint8_t F64_GE      = 0x66;

    // i32 arithmetic
    constexpr uint8_t I32_ADD     = 0x6a;
    constexpr uint8_t I32_SUB     = 0x6b;
    constexpr uint8_t I32_MUL     = 0x6c;
    constexpr uint8_t I32_DIV_S   = 0x6d;
    constexpr uint8_t I32_REM_S   = 0x6f;
    constexpr uint8_t I32_AND     = 0x71;
    constexpr uint8_t I32_OR      = 0x72;
    constexpr uint8_t I32_SHL     = 0x74;
    constexpr uint8_t I32_SHR_S   = 0x75;

    // f64 arithmetic
    constexpr uint8_t F64_ABS     = 0x99;
    constexpr uint8_t F64_NEG     = 0x9a;
    constexpr uint8_t F64_SQRT    = 0x9f;
    constexpr uint8_t F64_ADD     = 0xa0;
    constexpr uint8_t F64_SUB     = 0xa1;
    constexpr uint8_t F64_MUL     = 0xa2;
    constexpr uint8_t F64_DIV     = 0xa3;
    constexpr uint8_t F64_MIN     = 0xa4;
    constexpr uint8_t F64_MAX     = 0xa5;

    // conversions
    constexpr uint8_t I32_TRUNC_F64_S = 0xaa;
    constexpr uint8_t F64_CONVERT_I32_S = 0xb7;
}

// ---- WASM type byte codes ------------------------------------------------

namespace type {
    constexpr uint8_t I32  = 0x7f;
    constexpr uint8_t I64  = 0x7e;
    constexpr uint8_t F32  = 0x7d;
    constexpr uint8_t F64  = 0x7c;
    constexpr uint8_t FUNCREF = 0x70;
    constexpr uint8_t EXTERNREF = 0x6f;
    constexpr uint8_t FUNC = 0x60;     // type prefix
    constexpr uint8_t VOID = 0x40;     // empty result for blocks
}

// ---- Section ids ---------------------------------------------------------

namespace section {
    constexpr uint8_t CUSTOM   = 0;
    constexpr uint8_t TYPE     = 1;
    constexpr uint8_t IMPORT   = 2;
    constexpr uint8_t FUNCTION = 3;
    constexpr uint8_t TABLE    = 4;
    constexpr uint8_t MEMORY   = 5;
    constexpr uint8_t GLOBAL   = 6;
    constexpr uint8_t EXPORT   = 7;
    constexpr uint8_t START    = 8;
    constexpr uint8_t ELEMENT  = 9;
    constexpr uint8_t CODE     = 10;
    constexpr uint8_t DATA     = 11;
}

// ---- Export kind ---------------------------------------------------------

namespace export_kind {
    constexpr uint8_t FUNC   = 0x00;
    constexpr uint8_t TABLE  = 0x01;
    constexpr uint8_t MEMORY = 0x02;
    constexpr uint8_t GLOBAL = 0x03;
}

// -------------------------------------------------------------------------
// WasmWriter: an appendable byte buffer with the WASM-specific encoders.
// All integers in WASM use LEB128; floats are little-endian raw IEEE 754;
// strings are length-prefixed (LEB128) UTF-8.
// -------------------------------------------------------------------------

class WasmWriter {
public:
    void u8(uint8_t b) { buf_.push_back(b); }

    void u32_le(uint32_t v) {
        for (int i = 0; i < 4; ++i) buf_.push_back((uint8_t)((v >> (8 * i)) & 0xff));
    }

    // Unsigned LEB128: 7 bits per byte, high bit set if more follow.
    void uleb(uint64_t v) {
        do {
            uint8_t b = (uint8_t)(v & 0x7f);
            v >>= 7;
            if (v) b |= 0x80;
            buf_.push_back(b);
        } while (v);
    }

    // Signed LEB128: same shape but the sign bit of the last byte's
    // 6th bit (0x40) is implicitly extended.
    void sleb(int64_t v) {
        bool more = true;
        while (more) {
            uint8_t b = (uint8_t)(v & 0x7f);
            int64_t shifted = v >> 7;
            bool sign = (b & 0x40) != 0;
            if ((shifted == 0 && !sign) || (shifted == -1 && sign)) {
                more = false;
            } else {
                b |= 0x80;
            }
            v = shifted;
            buf_.push_back(b);
        }
    }

    void f64_le(double d) {
        uint8_t b[8];
        std::memcpy(b, &d, 8);
        for (int i = 0; i < 8; ++i) buf_.push_back(b[i]);
    }

    void name(const std::string& s) {
        uleb((uint64_t)s.size());
        for (char c : s) buf_.push_back((uint8_t)c);
    }

    void raw(const std::vector<uint8_t>& bytes) {
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    const std::vector<uint8_t>& bytes() const { return buf_; }
    std::vector<uint8_t> take() && { return std::move(buf_); }
    size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

// -------------------------------------------------------------------------
// A WASM function: a signature index + locals + body bytes. The locals
// vector is grouped runs of (count, type) which the body uses by index
// (params come first, locals after). For now we use only f64 and i32
// locals; expand types in lockstep with the codegen as needed.
// -------------------------------------------------------------------------

struct WasmFunc {
    uint32_t type_idx = 0;
    // (count, type) pairs.
    std::vector<std::pair<uint32_t, uint8_t>> locals;
    // Body bytes, NOT including the trailing END opcode (we add it in emit).
    std::vector<uint8_t> body;
};

// -------------------------------------------------------------------------
// WasmModule: high-level module builder. Track tables of types,
// functions, imports, exports; provide add_* methods that return the
// new entry's index; emit() serializes the whole thing to a byte vector
// that's ready to hand to WebAssembly.instantiate().
//
// Section emission order is the standard one (1 type, 2 import, 3
// function, ... 7 export, 10 code).
// -------------------------------------------------------------------------

class WasmModule {
public:
    // Add a func-type signature `(params) -> (results)`. Returns index.
    uint32_t add_type(const std::vector<uint8_t>& params,
                      const std::vector<uint8_t>& results) {
        WasmWriter w;
        w.u8(type::FUNC);
        w.uleb(params.size());
        for (uint8_t p : params) w.u8(p);
        w.uleb(results.size());
        for (uint8_t r : results) w.u8(r);
        types_.push_back(std::move(w).take());
        return (uint32_t)(types_.size() - 1);
    }

    // Import a function. Returns its function index (which counts ahead
    // of any local functions you add later -- imports always come first
    // in the function index space).
    uint32_t add_import_func(const std::string& mod, const std::string& nm,
                             uint32_t type_idx) {
        Import im;
        im.mod = mod;
        im.nm  = nm;
        im.kind = export_kind::FUNC;
        im.type_idx = type_idx;
        imports_.push_back(std::move(im));
        return n_func_imports_++;
    }

    // Add a local function definition. Returns the function index (which
    // starts after all imports).
    uint32_t add_function(WasmFunc f) {
        funcs_.push_back(std::move(f));
        return n_func_imports_ + (uint32_t)(funcs_.size() - 1);
    }

    // Add a memory section. WASM 1.0 allows at most one memory; pages
    // are 64KB. min/max are page counts; max = 0 means "no max."
    void set_memory(uint32_t min_pages, uint32_t max_pages = 0) {
        has_memory_ = true;
        mem_min_ = min_pages;
        mem_max_ = max_pages;
    }

    // Export an entity. kind is one of export_kind::*.
    void add_export(const std::string& nm, uint8_t kind, uint32_t idx) {
        Export ex;
        ex.nm = nm;
        ex.kind = kind;
        ex.idx = idx;
        exports_.push_back(std::move(ex));
    }

    // Serialize the whole module. Output is ready to hand to
    // WebAssembly.compile/instantiate.
    std::vector<uint8_t> emit() {
        WasmWriter w;
        // Magic + version 1.
        w.u8(0x00); w.u8(0x61); w.u8(0x73); w.u8(0x6d);
        w.u32_le(0x00000001);

        // Section 1: types.
        if (!types_.empty()) {
            WasmWriter s;
            s.uleb(types_.size());
            for (auto& t : types_) s.raw(t);
            emit_section(w, section::TYPE, s.bytes());
        }

        // Section 2: imports.
        if (!imports_.empty()) {
            WasmWriter s;
            s.uleb(imports_.size());
            for (auto& im : imports_) {
                s.name(im.mod);
                s.name(im.nm);
                s.u8(im.kind);
                // For func imports we encode the type index.
                if (im.kind == export_kind::FUNC) {
                    s.uleb(im.type_idx);
                } else {
                    // Other import kinds aren't used yet.
                }
            }
            emit_section(w, section::IMPORT, s.bytes());
        }

        // Section 3: function (type index per local function).
        if (!funcs_.empty()) {
            WasmWriter s;
            s.uleb(funcs_.size());
            for (auto& f : funcs_) s.uleb(f.type_idx);
            emit_section(w, section::FUNCTION, s.bytes());
        }

        // Section 5: memory.
        if (has_memory_) {
            WasmWriter s;
            s.uleb(1); // one memory
            if (mem_max_ > 0) {
                s.u8(0x01); // flags = has max
                s.uleb(mem_min_);
                s.uleb(mem_max_);
            } else {
                s.u8(0x00); // flags = no max
                s.uleb(mem_min_);
            }
            emit_section(w, section::MEMORY, s.bytes());
        }

        // Section 7: exports.
        if (!exports_.empty()) {
            WasmWriter s;
            s.uleb(exports_.size());
            for (auto& ex : exports_) {
                s.name(ex.nm);
                s.u8(ex.kind);
                s.uleb(ex.idx);
            }
            emit_section(w, section::EXPORT, s.bytes());
        }

        // Section 10: code (function bodies).
        if (!funcs_.empty()) {
            WasmWriter s;
            s.uleb(funcs_.size());
            for (auto& f : funcs_) {
                WasmWriter b;
                b.uleb(f.locals.size());
                for (auto& l : f.locals) {
                    b.uleb(l.first);
                    b.u8(l.second);
                }
                b.raw(f.body);
                b.u8(op::END);
                s.uleb(b.size());
                s.raw(b.bytes());
            }
            emit_section(w, section::CODE, s.bytes());
        }

        return std::move(w).take();
    }

private:
    struct Import {
        std::string mod, nm;
        uint8_t kind = 0;
        uint32_t type_idx = 0;
    };
    struct Export {
        std::string nm;
        uint8_t kind = 0;
        uint32_t idx = 0;
    };

    std::vector<std::vector<uint8_t>> types_;
    std::vector<Import> imports_;
    std::vector<WasmFunc> funcs_;
    std::vector<Export> exports_;
    uint32_t n_func_imports_ = 0;
    bool has_memory_ = false;
    uint32_t mem_min_ = 1;
    uint32_t mem_max_ = 0;

    static void emit_section(WasmWriter& out, uint8_t id,
                             const std::vector<uint8_t>& payload) {
        out.u8(id);
        out.uleb(payload.size());
        for (uint8_t b : payload) out.u8(b);
    }
};

} // namespace wasm
} // namespace knot
