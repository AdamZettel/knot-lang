#pragma once
#include "span.hpp"
#include <memory>
#include <string>
#include <vector>

namespace knot {

// All AST nodes are owned by std::unique_ptr from their parent.
// Every node carries a Span; every error message can point at source.

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// ---- Expressions -------------------------------------------------------

enum class ExprKind {
    NumberLit, StringLit, BoolLit, NilLit,
    VecLit,      // [1, 2, 3]
    MatLit,      // [[1,2],[3,4]]
    Ident,       // foo
    Unary,       // -x, !x
    Binary,      // x + y, x == y, x && y
    Index,       // v[i] or m[i, j] or v[a:b]
    Slice,       // a:b (only appears inside Index)
    Call,        // f(a, b)
    FnExpr,      // fn(x[, y, ...]) -> EXPR   (single-expression closure)
};

enum class UnOp { Neg, Not };
enum class BinOp {
    Add, Sub, Mul, Div, Mod, Matmul,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
};

struct Expr {
    ExprKind kind;
    Span span;

    // Discriminated payload. Only the fields matching `kind` are valid.
    double num = 0.0;
    bool boolean = false;
    std::string str;          // for StringLit and Ident
    UnOp unop = UnOp::Neg;
    BinOp binop = BinOp::Add;
    ExprPtr lhs, rhs;         // Unary uses rhs; Binary uses both
    std::vector<ExprPtr> elems;   // VecLit elements, Call args, Index indices
    std::vector<std::vector<ExprPtr>> rows; // MatLit rows
    ExprPtr callee;           // Call: the function expression
                              // Index: the thing being indexed
    // Slice fields (a:b). Either side may be null for open slices (`:n`, `n:`).
    ExprPtr slice_lo, slice_hi;

    // FnExpr: parameter names live in `params`, the body is a single
    // expression stored in `lhs` (we reuse the field rather than add
    // a dedicated one). Captures its surrounding env at eval time.
    std::vector<std::string> params;

    Expr(ExprKind k, Span s) : kind(k), span(s) {}
};

// ---- Statements --------------------------------------------------------

enum class StmtKind {
    Assign,     // x = expr;  or  v[i] = expr;
    CompAssign, // x += expr;  x -= expr;  etc.
    ExprStmt,   // expr;
    Print,      // print expr;
    If,         // if (cond) { ... } else { ... }
    While,      // while (cond) { ... }
    Loop,       // legacy: loop expr { ... }  or  loop expr as name { ... }
    For,        // for i to N | for x in v | for i, x in v
    Block,      // { stmts... }
    FnDecl,     // def name(params) { body }
    Return,     // return expr;
    Break,      // break;     (exits the nearest enclosing for/while/loop)
    Continue,   // continue;  (skips to the next iteration of same)
    TestDecl,   // test "name" { body }   (collected by --test; no-op otherwise)
};

// For-statement form. The parser decides which one at parse time based on
// the keyword (`to` vs `in`) and the comma in the binder list.
enum class ForForm {
    ToCount,    // for IDX to N       -> IDX = 0..N-1
    InElem,     // for ELEM in v      -> ELEM walks elements of v
    InBoth,     // for IDX, ELEM in v -> IDX = 0..len(v)-1, ELEM = v[IDX]
};

struct Stmt {
    StmtKind kind;
    Span span;

    std::string name;             // Assign target name (when target==null), FnDecl name, Loop index name
    ExprPtr expr;                 // value for Assign/Print/Return/ExprStmt; cond for If/While; over-expr for Loop/For
    ExprPtr target;               // for indexed assignment: v[i] = expr
    std::vector<StmtPtr> body;    // Block, If-then, While, Loop, For, FnDecl body
    std::vector<StmtPtr> else_body; // If-else
    std::vector<std::string> params; // FnDecl params
    std::vector<ExprPtr> param_defaults; // same length as params; null entries mean required
    BinOp comp_op = BinOp::Add;   // for CompAssign: which op (+,-,*,/)

    // For-statement fields. `name` (above) holds the first binder (index
    // for ToCount/InBoth, element for InElem); elem_name holds the second
    // binder for InBoth.
    ForForm for_form = ForForm::ToCount;
    std::string elem_name;

    Stmt(StmtKind k, Span s) : kind(k), span(s) {}
};

} // namespace knot
