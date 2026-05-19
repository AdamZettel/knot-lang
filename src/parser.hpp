#pragma once
#include "ast.hpp"
#include "diag.hpp"
#include "token.hpp"
#include <string>
#include <vector>

namespace knot {

class Parser {
    const std::vector<Token>& toks;
    size_t pos = 0;

public:
    explicit Parser(const std::vector<Token>& t) : toks(t) {}

    std::vector<StmtPtr> parse_program() {
        std::vector<StmtPtr> out;
        skip_terminators();
        while (!check(Tok::Eof)) {
            out.push_back(parse_stmt());
            skip_terminators();
        }
        return out;
    }

private:
    const Token& peek(size_t off = 0) const { return toks[pos + off]; }
    const Token& cur() const { return toks[pos]; }
    bool check(Tok k) const { return cur().kind == k; }
    bool match(Tok k) { if (check(k)) { ++pos; return true; } return false; }

    const Token& expect(Tok k, const char* what) {
        if (!check(k)) {
            std::string msg = std::string("expected ") + what + ", got "
                + tok_name(cur().kind);
            throw Diag(cur().span, msg);
        }
        return toks[pos++];
    }

    // Eat any run of statement terminators (newlines and/or semicolons).
    void skip_terminators() {
        while (check(Tok::Newline) || check(Tok::Semicolon)) ++pos;
    }

    // Consume one statement terminator (or accept EOF/`}`).
    void expect_terminator(const char* what) {
        if (check(Tok::Newline) || check(Tok::Semicolon)) { ++pos; return; }
        if (check(Tok::Eof) || check(Tok::RBrace)) return;
        throw Diag(cur().span,
            std::string("expected newline or ';' after ") + what
            + ", got " + tok_name(cur().kind));
    }

    // ---- Statements ------------------------------------------------------

    StmtPtr parse_stmt() {
        if (check(Tok::If))     return parse_if();
        if (check(Tok::While))  return parse_while();
        if (check(Tok::Loop))   return parse_loop();
        if (check(Tok::For))    return parse_for();
        if (check(Tok::Def))    return parse_def();
        if (check(Tok::Return)) return parse_return();
        if (check(Tok::LBrace)) return parse_block();

        // Expression statement, plain assignment, or compound assignment.
        Span start = cur().span;
        ExprPtr e = parse_expr();

        // Compound assignment first (these only make sense for simple targets).
        auto compound = [&](BinOp op) -> StmtPtr {
            ++pos;
            ExprPtr value = parse_expr();
            expect_terminator("compound assignment");
            auto s = std::make_unique<Stmt>(StmtKind::CompAssign,
                Span::merge(start, value->span));
            s->comp_op = op;
            if (e->kind == ExprKind::Ident) {
                s->name = e->str;
            } else if (e->kind == ExprKind::Index) {
                s->target = std::move(e);
            } else {
                throw Diag(e->span, "left-hand side of compound assignment must be a name or index");
            }
            s->expr = std::move(value);
            return s;
        };
        if (check(Tok::PlusEq))  return compound(BinOp::Add);
        if (check(Tok::MinusEq)) return compound(BinOp::Sub);
        if (check(Tok::StarEq))  return compound(BinOp::Mul);
        if (check(Tok::SlashEq)) return compound(BinOp::Div);

        if (match(Tok::Eq)) {
            ExprPtr value = parse_expr();
            expect_terminator("assignment");
            auto s = std::make_unique<Stmt>(StmtKind::Assign,
                Span::merge(start, value->span));
            if (e->kind == ExprKind::Ident) {
                s->name = e->str;
            } else if (e->kind == ExprKind::Index) {
                s->target = std::move(e);
            } else {
                throw Diag(e->span, "left-hand side of '=' must be a name or index");
            }
            s->expr = std::move(value);
            return s;
        }

        expect_terminator("expression");
        auto s = std::make_unique<Stmt>(StmtKind::ExprStmt, e->span);
        s->expr = std::move(e);
        return s;
    }

    StmtPtr parse_if() {
        Span start = cur().span;
        ++pos; // 'if'
        ExprPtr cond = parse_paren_or_bare_cond("if");
        std::vector<StmtPtr> then_body = parse_block_body();
        std::vector<StmtPtr> else_body;
        skip_terminators_before_else();
        if (match(Tok::Else)) {
            if (check(Tok::If)) {
                else_body.push_back(parse_if());
            } else {
                else_body = parse_block_body();
            }
        }
        auto s = std::make_unique<Stmt>(StmtKind::If, start);
        s->expr = std::move(cond);
        s->body = std::move(then_body);
        s->else_body = std::move(else_body);
        return s;
    }

    // `else` may appear on the next line; consume any newlines between `}` and `else`.
    void skip_terminators_before_else() {
        size_t save = pos;
        while (check(Tok::Newline) || check(Tok::Semicolon)) ++pos;
        if (!check(Tok::Else)) pos = save;
    }

    StmtPtr parse_while() {
        Span start = cur().span;
        ++pos; // 'while'
        ExprPtr cond = parse_paren_or_bare_cond("while");
        std::vector<StmtPtr> body = parse_block_body();
        auto s = std::make_unique<Stmt>(StmtKind::While, start);
        s->expr = std::move(cond);
        s->body = std::move(body);
        return s;
    }

    // loop EXPR { ... }            -- index variable is implicit `it`
    // loop EXPR as NAME { ... }    -- index variable is NAME
    // EXPR is either a number n (loop 0..n-1) or a vec (loop over elements).
    StmtPtr parse_loop() {
        Span start = cur().span;
        ++pos; // 'loop'
        ExprPtr over = parse_expr();
        std::string name = "it";
        if (match(Tok::As)) {
            const Token& nm = expect(Tok::Ident, "name after 'as'");
            name = nm.text;
        }
        std::vector<StmtPtr> body = parse_block_body();
        auto s = std::make_unique<Stmt>(StmtKind::Loop, start);
        s->expr = std::move(over);
        s->name = name;
        s->body = std::move(body);
        return s;
    }

    // for IDENT to EXPR { ... }           -- numerical count, IDENT = 0..EXPR-1
    // for IDENT in EXPR { ... }           -- element foreach, IDENT walks v
    // for IDENT, IDENT in EXPR { ... }    -- index + element pair
    //
    // The form is decided by what comes after the first IDENT (or pair):
    // `to` -> ToCount, `in` (with one binder) -> InElem, `in` (with two
    // binders) -> InBoth. The parser doesn't try to be cute about deciding
    // -- it commits based on the keyword it sees.
    StmtPtr parse_for() {
        Span start = cur().span;
        ++pos; // 'for'

        const Token& first = expect(Tok::Ident,
            "name after 'for' (e.g. 'for i to N')");
        std::string idx_name = first.text;
        std::string elem_name;

        bool have_two_binders = false;
        if (match(Tok::Comma)) {
            const Token& second = expect(Tok::Ident,
                "name after ',' in for-binders (e.g. 'for i, x in v')");
            elem_name = second.text;
            have_two_binders = true;
        }

        ForForm form;
        ExprPtr bound;
        if (have_two_binders) {
            // Two binders only works with `in`. `for i, x to N` is ill-formed.
            expect(Tok::In, "'in' after two for-binders (got something else)");
            bound = parse_expr();
            form = ForForm::InBoth;
        } else if (match(Tok::To)) {
            bound = parse_expr();
            form = ForForm::ToCount;
        } else if (match(Tok::In)) {
            bound = parse_expr();
            form = ForForm::InElem;
        } else {
            throw Diag(cur().span,
                "expected 'to' or 'in' after for-binder, got "
                + std::string(tok_name(cur().kind)));
        }

        std::vector<StmtPtr> body = parse_block_body();

        auto s = std::make_unique<Stmt>(StmtKind::For, start);
        s->for_form = form;
        s->name = idx_name;       // for InElem this is actually the elem;
                                  // for ToCount/InBoth it's the index.
        s->elem_name = elem_name; // only used for InBoth
        s->expr = std::move(bound);
        s->body = std::move(body);
        return s;
    }

    // Accept either `if (cond) { ... }` or `if cond { ... }`.
    // Parse the condition of if/while. We just parse a full expression; the
    // expression naturally terminates at the `{` that starts the body. This
    // means `while (q + 1) * b <= a {` parses correctly (the outer parens
    // are part of the expression, not a cond wrapper).
    ExprPtr parse_paren_or_bare_cond(const char*) {
        return parse_expr();
    }

    StmtPtr parse_def() {
        Span start = cur().span;
        ++pos; // 'def'
        const Token& name = expect(Tok::Ident, "function name");
        expect(Tok::LParen, "'(' in function declaration");
        std::vector<std::string> params;
        std::vector<ExprPtr> defaults;
        if (!check(Tok::RParen)) {
            parse_param(params, defaults);
            while (match(Tok::Comma)) parse_param(params, defaults);
        }
        expect(Tok::RParen, "')' in function declaration");
        // Accept an optional `:` after the header to ease the eye for
        // Python users; we still require braces.
        match(Tok::Colon);
        std::vector<StmtPtr> body = parse_block_body();
        auto s = std::make_unique<Stmt>(StmtKind::FnDecl, start);
        s->name = name.text;
        s->params = std::move(params);
        s->param_defaults = std::move(defaults);
        s->body = std::move(body);
        return s;
    }

    void parse_param(std::vector<std::string>& params,
                     std::vector<ExprPtr>& defaults) {
        const Token& p = expect(Tok::Ident, "parameter name");
        params.push_back(p.text);
        if (match(Tok::Eq)) {
            defaults.push_back(parse_expr());
        } else {
            defaults.push_back(nullptr);
        }
    }

    StmtPtr parse_return() {
        Span start = cur().span;
        ++pos; // 'return'
        auto s = std::make_unique<Stmt>(StmtKind::Return, start);
        if (!check(Tok::Newline) && !check(Tok::Semicolon)
         && !check(Tok::Eof) && !check(Tok::RBrace)) {
            s->expr = parse_expr();
        }
        expect_terminator("return");
        return s;
    }

    StmtPtr parse_block() {
        Span start = cur().span;
        std::vector<StmtPtr> body = parse_block_body();
        auto s = std::make_unique<Stmt>(StmtKind::Block, start);
        s->body = std::move(body);
        return s;
    }

    std::vector<StmtPtr> parse_block_body() {
        expect(Tok::LBrace, "'{'");
        skip_terminators();
        std::vector<StmtPtr> out;
        while (!check(Tok::RBrace) && !check(Tok::Eof)) {
            out.push_back(parse_stmt());
            skip_terminators();
        }
        expect(Tok::RBrace, "'}'");
        return out;
    }

    // ---- Expressions: Pratt-style precedence climbing --------------------
    //
    //   1: or  (`or`, `||`)
    //   2: and (`and`, `&&`)
    //   3: not (prefix)
    //   4: == !=
    //   5: < <= > >=
    //   6: + -
    //   7: * / % @
    //   8: unary - !
    //   9: postfix call f(args), index a[i]
    //  10: primary

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        ExprPtr lhs = parse_and();
        while (check(Tok::OrOr)) {
            ++pos;
            ExprPtr rhs = parse_and();
            lhs = make_binary(BinOp::Or, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    ExprPtr parse_and() {
        ExprPtr lhs = parse_equality();
        while (check(Tok::AndAnd)) {
            ++pos;
            ExprPtr rhs = parse_equality();
            lhs = make_binary(BinOp::And, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    ExprPtr parse_equality() {
        ExprPtr lhs = parse_compare();
        while (check(Tok::EqEq) || check(Tok::BangEq)) {
            BinOp op = check(Tok::EqEq) ? BinOp::Eq : BinOp::Ne;
            ++pos;
            ExprPtr rhs = parse_compare();
            lhs = make_binary(op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    ExprPtr parse_compare() {
        ExprPtr lhs = parse_add();
        while (check(Tok::Lt) || check(Tok::LtEq) || check(Tok::Gt) || check(Tok::GtEq)) {
            BinOp op;
            switch (cur().kind) {
                case Tok::Lt:   op = BinOp::Lt; break;
                case Tok::LtEq: op = BinOp::Le; break;
                case Tok::Gt:   op = BinOp::Gt; break;
                default:        op = BinOp::Ge; break;
            }
            ++pos;
            ExprPtr rhs = parse_add();
            lhs = make_binary(op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    ExprPtr parse_add() {
        ExprPtr lhs = parse_mul();
        while (check(Tok::Plus) || check(Tok::Minus)) {
            BinOp op = check(Tok::Plus) ? BinOp::Add : BinOp::Sub;
            ++pos;
            ExprPtr rhs = parse_mul();
            lhs = make_binary(op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    ExprPtr parse_mul() {
        ExprPtr lhs = parse_unary();
        while (check(Tok::Star) || check(Tok::Slash) || check(Tok::Percent) || check(Tok::At)) {
            BinOp op;
            switch (cur().kind) {
                case Tok::Star:    op = BinOp::Mul; break;
                case Tok::Slash:   op = BinOp::Div; break;
                case Tok::Percent: op = BinOp::Mod; break;
                default:           op = BinOp::Matmul; break;
            }
            ++pos;
            ExprPtr rhs = parse_unary();
            lhs = make_binary(op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    ExprPtr parse_unary() {
        if (check(Tok::Minus) || check(Tok::Bang)) {
            Span start = cur().span;
            UnOp op = check(Tok::Minus) ? UnOp::Neg : UnOp::Not;
            ++pos;
            ExprPtr rhs = parse_unary();
            auto e = std::make_unique<Expr>(ExprKind::Unary,
                Span::merge(start, rhs->span));
            e->unop = op;
            e->rhs = std::move(rhs);
            return e;
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        ExprPtr e = parse_primary();
        while (true) {
            if (check(Tok::LParen)) {
                Span start = e->span;
                ++pos;
                std::vector<ExprPtr> args;
                if (!check(Tok::RParen)) {
                    args.push_back(parse_expr());
                    while (match(Tok::Comma)) args.push_back(parse_expr());
                }
                Span end = cur().span;
                expect(Tok::RParen, "')' in call");
                auto call = std::make_unique<Expr>(ExprKind::Call,
                    Span::merge(start, end));
                call->callee = std::move(e);
                call->elems = std::move(args);
                e = std::move(call);
            } else if (check(Tok::LBracket)) {
                Span start = e->span;
                ++pos;
                std::vector<ExprPtr> idx;
                idx.push_back(parse_index_part());
                while (match(Tok::Comma)) idx.push_back(parse_index_part());
                Span end = cur().span;
                expect(Tok::RBracket, "']' after index");
                auto ix = std::make_unique<Expr>(ExprKind::Index,
                    Span::merge(start, end));
                ix->callee = std::move(e);
                ix->elems = std::move(idx);
                e = std::move(ix);
            } else {
                break;
            }
        }
        return e;
    }

    // Inside [ ]: either an expression, or a slice `a:b`, `:b`, `a:`, or `:`.
    ExprPtr parse_index_part() {
        Span start = cur().span;
        // Open-lo slice: starts with ':'
        if (check(Tok::Colon)) {
            ++pos;
            auto e = std::make_unique<Expr>(ExprKind::Slice, start);
            if (!check(Tok::Comma) && !check(Tok::RBracket)) {
                e->slice_hi = parse_expr();
                e->span = Span::merge(start, e->slice_hi->span);
            }
            return e;
        }
        ExprPtr lo = parse_expr();
        if (check(Tok::Colon)) {
            ++pos;
            auto e = std::make_unique<Expr>(ExprKind::Slice, lo->span);
            e->slice_lo = std::move(lo);
            if (!check(Tok::Comma) && !check(Tok::RBracket)) {
                e->slice_hi = parse_expr();
                e->span = Span::merge(e->slice_lo->span, e->slice_hi->span);
            }
            return e;
        }
        return lo;
    }

    ExprPtr parse_primary() {
        const Token& t = cur();
        switch (t.kind) {
            case Tok::Number: {
                ++pos;
                auto e = std::make_unique<Expr>(ExprKind::NumberLit, t.span);
                e->num = t.number;
                return e;
            }
            case Tok::String: {
                ++pos;
                auto e = std::make_unique<Expr>(ExprKind::StringLit, t.span);
                e->str = t.text;
                return e;
            }
            case Tok::True: {
                ++pos;
                auto e = std::make_unique<Expr>(ExprKind::BoolLit, t.span);
                e->boolean = true;
                return e;
            }
            case Tok::False: {
                ++pos;
                auto e = std::make_unique<Expr>(ExprKind::BoolLit, t.span);
                e->boolean = false;
                return e;
            }
            case Tok::None_: {
                ++pos;
                return std::make_unique<Expr>(ExprKind::NilLit, t.span);
            }
            case Tok::Ident: {
                ++pos;
                auto e = std::make_unique<Expr>(ExprKind::Ident, t.span);
                e->str = t.text;
                return e;
            }
            case Tok::LParen: {
                ++pos;
                ExprPtr inner = parse_expr();
                expect(Tok::RParen, "')'");
                return inner;
            }
            case Tok::LBracket: {
                Span start = t.span;
                ++pos;
                if (check(Tok::RBracket)) {
                    ++pos;
                    auto e = std::make_unique<Expr>(ExprKind::VecLit, start);
                    return e;
                }
                if (check(Tok::LBracket)) {
                    auto e = std::make_unique<Expr>(ExprKind::MatLit, start);
                    while (true) {
                        expect(Tok::LBracket, "'[' starting matrix row");
                        std::vector<ExprPtr> row;
                        if (!check(Tok::RBracket)) {
                            row.push_back(parse_expr());
                            while (match(Tok::Comma)) row.push_back(parse_expr());
                        }
                        expect(Tok::RBracket, "']' ending matrix row");
                        e->rows.push_back(std::move(row));
                        if (!match(Tok::Comma)) break;
                    }
                    Span end = cur().span;
                    expect(Tok::RBracket, "']' ending matrix literal");
                    e->span = Span::merge(start, end);
                    return e;
                }
                auto e = std::make_unique<Expr>(ExprKind::VecLit, start);
                e->elems.push_back(parse_expr());
                while (match(Tok::Comma)) e->elems.push_back(parse_expr());
                Span end = cur().span;
                expect(Tok::RBracket, "']' ending vector literal");
                e->span = Span::merge(start, end);
                return e;
            }
            default:
                throw Diag(t.span, std::string("unexpected ") + tok_name(t.kind)
                    + " in expression");
        }
    }

    ExprPtr make_binary(BinOp op, ExprPtr l, ExprPtr r) {
        Span s = Span::merge(l->span, r->span);
        auto e = std::make_unique<Expr>(ExprKind::Binary, s);
        e->binop = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }
};

} // namespace knot
