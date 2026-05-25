#pragma once
#include "ast.hpp"
#include "linalg.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

namespace knot {

struct Value;
struct Env;
using EnvPtr = std::shared_ptr<Env>;

// (ShapeTag is defined in linalg.hpp so Vec and Mat can carry tags too.)

// A user-defined function value. Captures the environment at definition
// site so closures work. The body and defaults are borrowed from the AST
// (which outlives execution), hence the raw pointers.
//
// Two body shapes are supported:
//   - statement-list body (`def NAME(params) { ... }`): `body` points
//     at the statement vector and `expr_body` is null.
//   - expression body (`fn(x) -> EXPR`): `expr_body` points at the
//     single expression and `body` is null. Calling such a function
//     evaluates the expression in a fresh scope and returns its
//     value; there's no implicit `return`.
struct Function {
    std::vector<std::string> params;
    const std::vector<StmtPtr>* body = nullptr;
    const Expr*                 expr_body = nullptr;
    const std::vector<ExprPtr>* param_defaults = nullptr; // same length as params
    EnvPtr closure;
    std::string name; // for error messages
    std::string docstring; // first string-literal expression-stmt in body
};

// A builtin: just a C++ function pointer wrapped up. Takes a vector of
// arguments and a span (for error reporting) and returns a Value.
using BuiltinFn = Value(*)(const std::vector<Value>&, Span);

struct Builtin {
    std::string name;
    BuiltinFn fn;
};

// THE value type. Everything the interpreter computes is one of these.
// std::variant gives us a tagged union with proper RAII; std::shared_ptr
// for Vec/Mat keeps copies cheap (and matters once arrays get large).
struct Value;
using ValueList = std::vector<Value>;

struct Value {
    using Storage = std::variant<
        std::monostate,                  // Nil
        double,                          // Num
        bool,                            // Bool
        std::string,                     // Str
        std::shared_ptr<Vec>,            // Vec (numerical column vector)
        std::shared_ptr<Mat>,            // Mat (column-major matrix)
        std::shared_ptr<Tensor>,         // Tensor (N-D, row-major, rank > 2)
        std::shared_ptr<ValueList>,      // List (heterogeneous, for return tuples)
        std::shared_ptr<Function>,       // user-defined function
        Builtin                          // builtin function
    >;
    Storage v;
    std::shared_ptr<ShapeTag> tag;

    // Constructors for ergonomics.
    Value() : v(std::monostate{}) {}
    static Value nil()                          { return Value{}; }
    static Value num(double x)                  { Value r; r.v = x; return r; }
    static Value tagged_num(double x, ShapeTag t) {
        Value r; r.v = x;
        r.tag = std::make_shared<ShapeTag>(std::move(t));
        return r;
    }
    static Value boolean(bool b)                { Value r; r.v = b; return r; }
    static Value str(std::string s)             { Value r; r.v = std::move(s); return r; }
    static Value vec(Vec x) {
        Value r;
        auto sp = std::make_shared<Vec>(std::move(x));
        if (!sp->len_tag) {
            // Default length tag: identifies this vec.
            auto t = std::make_shared<ShapeTag>();
            t->container_id = std::to_string((uintptr_t)sp.get());
            t->axis = -1;
            t->display = "len(vec)";
            sp->len_tag = std::move(t);
        }
        r.v = std::move(sp);
        return r;
    }
    static Value mat(Mat x) {
        Value r;
        auto sp = std::make_shared<Mat>(std::move(x));
        if (!sp->row_tag) {
            auto t = std::make_shared<ShapeTag>();
            t->container_id = std::to_string((uintptr_t)sp.get());
            t->axis = 0;
            t->display = "rows(mat)";
            sp->row_tag = std::move(t);
        }
        if (!sp->col_tag) {
            auto t = std::make_shared<ShapeTag>();
            t->container_id = std::to_string((uintptr_t)sp.get());
            t->axis = 1;
            t->display = "cols(mat)";
            sp->col_tag = std::move(t);
        }
        r.v = std::move(sp);
        return r;
    }
    static Value tensor(Tensor t) {
        Value r;
        r.v = std::make_shared<Tensor>(std::move(t));
        return r;
    }
    static Value fn(std::shared_ptr<Function> f){ Value r; r.v = std::move(f); return r; }
    static Value builtin(Builtin b)             { Value r; r.v = std::move(b); return r; }
    static Value list(ValueList x) {
        Value r; r.v = std::make_shared<ValueList>(std::move(x)); return r;
    }

    // Type queries.
    bool is_nil()    const { return std::holds_alternative<std::monostate>(v); }
    bool is_num()    const { return std::holds_alternative<double>(v); }
    bool is_bool()   const { return std::holds_alternative<bool>(v); }
    bool is_str()    const { return std::holds_alternative<std::string>(v); }
    bool is_vec()    const { return std::holds_alternative<std::shared_ptr<Vec>>(v); }
    bool is_mat()    const { return std::holds_alternative<std::shared_ptr<Mat>>(v); }
    bool is_tensor() const { return std::holds_alternative<std::shared_ptr<Tensor>>(v); }
    bool is_list()   const { return std::holds_alternative<std::shared_ptr<ValueList>>(v); }
    bool is_fn()     const { return std::holds_alternative<std::shared_ptr<Function>>(v); }
    bool is_builtin()const { return std::holds_alternative<Builtin>(v); }
    bool is_callable() const { return is_fn() || is_builtin(); }

    double          as_num()  const { return std::get<double>(v); }
    bool            as_bool() const { return std::get<bool>(v); }
    const std::string& as_str() const { return std::get<std::string>(v); }
    const Vec&      as_vec()  const { return *std::get<std::shared_ptr<Vec>>(v); }
    const Mat&      as_mat()  const { return *std::get<std::shared_ptr<Mat>>(v); }
    const Tensor&   as_tensor() const { return *std::get<std::shared_ptr<Tensor>>(v); }
    const ValueList& as_list() const { return *std::get<std::shared_ptr<ValueList>>(v); }
    const Function& as_fn()   const { return *std::get<std::shared_ptr<Function>>(v); }
    const Builtin&  as_builtin() const { return std::get<Builtin>(v); }

    const char* type_name() const {
        if (is_nil())     return "nil";
        if (is_num())     return "num";
        if (is_bool())    return "bool";
        if (is_str())     return "str";
        if (is_vec())     return "vec";
        if (is_mat())     return "mat";
        if (is_tensor())  return "tensor";
        if (is_list())    return "list";
        if (is_fn())      return "fn";
        if (is_builtin()) return "builtin";
        return "?";
    }
};

// Environment: a chain of frames. Each frame is a name->Value map; lookup
// walks up `parent`. Functions capture an EnvPtr at definition time.
struct Env {
    std::unordered_map<std::string, Value> vars;
    EnvPtr parent;
    // True for the frame at a function-call boundary: reads still walk
    // up to find stdlib / module globals, but writes stop at this frame
    // and create a local instead of clobbering the outer binding. The
    // sub-frames a function spawns (for, while, blocks) do not set this;
    // they let assignment walk back up to the function's locals.
    bool is_function_boundary = false;

    Env() = default;
    explicit Env(EnvPtr p) : parent(std::move(p)) {}

    Value* find(const std::string& name) {
        auto it = vars.find(name);
        if (it != vars.end()) return &it->second;
        if (parent) return parent->find(name);
        return nullptr;
    }

    void define(const std::string& name, Value val) {
        vars[name] = std::move(val);
    }

    // Read a binding by name. Walks the chain. Throws if not found.
    Value get(const std::string& name) {
        Value* p = find(name);
        if (!p) throw std::runtime_error("name not found: " + name);
        return *p;
    }

    // Read-only access to this frame's bindings (used by the REPL's `lslib`).
    const std::unordered_map<std::string, Value>& bindings() const { return vars; }

    // Walks the chain to find an existing binding to update. Stops at
    // function boundaries -- so a local `s = 0` inside `def sum(v)`
    // doesn't clobber a top-level `s` of the calling script. Returns
    // false if not found before the boundary or the chain ends; the
    // caller (StmtKind::Assign) then defines a new local in the current
    // frame.
    bool assign(const std::string& name, Value val) {
        auto it = vars.find(name);
        if (it != vars.end()) { it->second = std::move(val); return true; }
        if (is_function_boundary) return false;
        if (parent) return parent->assign(name, std::move(val));
        return false;
    }
};

} // namespace knot
