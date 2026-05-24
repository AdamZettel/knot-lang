#pragma once
#include "ast.hpp"
#include "diag.hpp"
#include "phrases.hpp"
#include "token.hpp"
#include <iostream>
#include <string>
#include <vector>

namespace knot {

class Parser {
    const std::vector<Token>& toks;
    // Source text (used only for source-slice queries like
    // labeling the args of `show`). Optional; when null the parser
    // falls back to a tag based on the first token's text.
    const std::string* source = nullptr;
    size_t pos = 0;

public:
    explicit Parser(const std::vector<Token>& t) : toks(t) {}
    Parser(const std::vector<Token>& t, const std::string& src)
        : toks(t), source(&src) {}

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
        if (check(Tok::Break))  return parse_break();
        if (check(Tok::Continue)) return parse_continue();
        if (check(Tok::Test))    return parse_test();
        if (check(Tok::Show))    return parse_show();
        if (check(Tok::Narrate)) return parse_narrate();
        if (check(Tok::Iterate)) return parse_iterate();
        if (check(Tok::Repeat))  return parse_repeat();
        if (check(Tok::LBrace))  return parse_block();

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

    StmtPtr parse_break() {
        Span start = cur().span;
        ++pos; // 'break'
        expect_terminator("break");
        return std::make_unique<Stmt>(StmtKind::Break, start);
    }

    StmtPtr parse_continue() {
        Span start = cur().span;
        ++pos; // 'continue'
        expect_terminator("continue");
        return std::make_unique<Stmt>(StmtKind::Continue, start);
    }

    // test "name" { body }    -- a named, isolated test block. Collected and
    // run by `--test`; skipped (no-op) under normal --interp or --exec.
    StmtPtr parse_test() {
        Span start = cur().span;
        ++pos; // 'test'
        const Token& name = expect(Tok::String, "string after 'test' (the test name)");
        std::vector<StmtPtr> body = parse_block_body();
        auto s = std::make_unique<Stmt>(StmtKind::TestDecl, start);
        s->name = name.text;
        s->body = std::move(body);
        return s;
    }

    // narrate EXPR   -- inline algorithm narration. Evaluates EXPR
    // (a string-valued expression) and prints "# <text>" to stdout
    // when narration is enabled (default on). The "why" beside the
    // "what" -- a programmer reading the code sees the algorithm
    // narrate itself as the program runs.
    StmtPtr parse_narrate() {
        Span start = cur().span;
        ++pos; // 'narrate'
        auto s = std::make_unique<Stmt>(StmtKind::Narrate, start);
        s->expr = parse_expr();
        expect_terminator("narrate");
        return s;
    }

    // Statement-level plainspeak phrases. Each desugars directly to
    // an existing control-flow AST node (StmtKind::For) and emits a
    // translation hint at parse time so the user sees the desugared
    // form they could have typed.
    //
    // iterate over EXPR as IDENT { body }
    //     -> for IDENT in EXPR { body }
    // iterate from A to B as IDENT { body }
    //     -> for IDENT in arange(A, B + 1) { body }   (inclusive)
    StmtPtr parse_iterate() {
        Span start = cur().span;
        ++pos; // 'iterate'

        // Distinguish the two forms by the literal Ident following
        // `iterate`. `over` and `from` aren't keywords, so we compare
        // by text.
        if (check(Tok::Ident) && (cur().text == "over" || cur().text == "from")) {
            std::string lead = cur().text;
            ++pos;
            if (lead == "over") return parse_iterate_over_tail(start);
            return parse_iterate_from_tail(start);
        }
        throw Diag(cur().span,
            "expected `over` or `from` after `iterate`, got "
            + std::string(tok_name(cur().kind)));
    }

    StmtPtr parse_iterate_over_tail(Span start) {
        ExprPtr coll = parse_expr();
        expect(Tok::As, "`as` after `iterate over EXPR`");
        const Token& nm = expect(Tok::Ident,
            "loop variable name after `as`");
        std::string ident = nm.text;
        std::vector<StmtPtr> body = parse_block_body();

        emit_iterate_hint(start, "iterate over",
            "for " + ident + " in <expr> { ... }");

        auto s = std::make_unique<Stmt>(StmtKind::For, start);
        s->for_form = ForForm::InElem;
        s->name = ident;
        s->expr = std::move(coll);
        s->body = std::move(body);
        return s;
    }

    StmtPtr parse_iterate_from_tail(Span start) {
        ExprPtr lo = parse_expr();
        expect(Tok::To, "`to` after `iterate from A`");
        ExprPtr hi = parse_expr();
        expect(Tok::As, "`as` after `iterate from A to B`");
        const Token& nm = expect(Tok::Ident,
            "loop variable name after `as`");
        std::string ident = nm.text;
        std::vector<StmtPtr> body = parse_block_body();

        emit_iterate_hint(start, "iterate from",
            "for " + ident + " in arange(<lo>, <hi> + 1) { ... }");

        // Build: for IDENT in arange(LO, HI + 1) { body }
        // The `+ 1` is added at parse time so `from A to B` is
        // inclusive of both endpoints, matching math-paper notation.
        Span hi_span = hi->span;
        auto one = std::make_unique<Expr>(ExprKind::NumberLit, hi_span);
        one->num = 1.0;
        auto hi_plus_one = std::make_unique<Expr>(ExprKind::Binary, hi_span);
        hi_plus_one->binop = BinOp::Add;
        hi_plus_one->lhs = std::move(hi);
        hi_plus_one->rhs = std::move(one);

        auto callee = std::make_unique<Expr>(ExprKind::Ident, start);
        callee->str = "arange";
        auto arange_call = std::make_unique<Expr>(ExprKind::Call, start);
        arange_call->callee = std::move(callee);
        arange_call->elems.push_back(std::move(lo));
        arange_call->elems.push_back(std::move(hi_plus_one));

        auto s = std::make_unique<Stmt>(StmtKind::For, start);
        s->for_form = ForForm::InElem;
        s->name = ident;
        s->expr = std::move(arange_call);
        s->body = std::move(body);
        return s;
    }

    // repeat EXPR times { body }
    //     -> for _ to EXPR { body }
    StmtPtr parse_repeat() {
        Span start = cur().span;
        ++pos; // 'repeat'
        ExprPtr count = parse_expr();
        // `times` is a literal word, not a keyword token.
        if (!check(Tok::Ident) || cur().text != "times") {
            throw Diag(cur().span,
                "expected `times` after `repeat EXPR`, got "
                + std::string(tok_name(cur().kind)));
        }
        ++pos; // 'times'
        std::vector<StmtPtr> body = parse_block_body();

        emit_iterate_hint(start, "repeat", "for _ to <count> { ... }");

        auto s = std::make_unique<Stmt>(StmtKind::For, start);
        s->for_form = ForForm::ToCount;
        s->name = "_";
        s->expr = std::move(count);
        s->body = std::move(body);
        return s;
    }

    // Helper: emit a "# <typed-phrase>  ->  <canonical>" line to
    // stderr if hints are on, slicing the user's source for the
    // typed form (covering everything from the leading keyword to
    // the opening brace, exclusive).
    void emit_iterate_hint(Span start, const char* /*kind*/, const std::string& canonical) {
        if (!phrase_hints_enabled() || !source) return;
        // Find the LBrace that opens the body -- it's the first one
        // we passed (already consumed). Walk back to find it via toks.
        // Easier: slice from start.start to the most-recent LBrace's
        // span.start. Since parse_block_body() already consumed the
        // brace, we look back for it.
        size_t look = pos;
        while (look > 0 && toks[look - 1].kind != Tok::LBrace) --look;
        // look now points just past the LBrace; walk back one to
        // get the brace itself.
        if (look == 0) return;
        size_t brace_idx = look - 1;
        size_t end = toks[brace_idx].span.start;
        if (end <= start.start || end > source->size()) return;
        std::string typed = source->substr(start.start, end - start.start);
        // Trim trailing whitespace.
        while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\n' || typed.back() == '\t'))
            typed.pop_back();
        std::cerr << "# " << typed << "  ->  " << canonical << "\n";
    }

    // show EXPR [, EXPR ...]   -- debug-print each expression as
    // "<source text>: <value>". The label for each arg is the
    // verbatim source slice the user typed, captured at parse time;
    // mirrors the `{x=}` shorthand other languages have but without
    // requiring a special format-string syntax.
    StmtPtr parse_show() {
        Span start = cur().span;
        ++pos; // 'show'
        auto s = std::make_unique<Stmt>(StmtKind::Show, start);

        // Parse comma-separated expressions, capturing each one's
        // source slice for the label. Bare `show` with no args
        // prints a blank line, mirroring `print()` with no args.
        if (!check(Tok::Newline) && !check(Tok::Semicolon)
         && !check(Tok::Eof) && !check(Tok::RBrace)) {
            s->show_labels.push_back(label_for(parse_show_one(s)));
            while (match(Tok::Comma)) {
                s->show_labels.push_back(label_for(parse_show_one(s)));
            }
        }
        expect_terminator("show");
        return s;
    }

    // Helper: parse one expression for a show stmt, push it into the
    // stmt's show_exprs, and return its span so the caller can
    // extract the label text. Kept separate from label_for() to keep
    // the parsing and labeling concerns visible at the call site.
    Span parse_show_one(std::unique_ptr<Stmt>& s) {
        ExprPtr e = parse_expr();
        Span sp = e->span;
        s->show_exprs.push_back(std::move(e));
        return sp;
    }

    // Extract the verbatim source text for the given span. Falls back
    // to "<expr>" if no source is available (in re-parsed phrase-hole
    // sub-parsers, for example).
    std::string label_for(Span sp) const {
        if (!source) return "<expr>";
        if (sp.start + sp.length > source->size()) return "<expr>";
        return source->substr(sp.start, sp.length);
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

public:
    // Walk an expression collecting identifier names that look like
    // free variables -- references to names that are NOT in the
    // built-in / stdlib known set. Used to derive the parameter name
    // for closure-shaped phrase holes: in
    //   take the integral of sin(x) from 0 to pi
    // the hole `sin(x)` contains a reference to `sin` (known) and
    // `x` (free). The free-var set is `{x}`, so we wrap the hole as
    // `fn(x) -> sin(x)`.
    //
    // FnExpr parameter names shadow their references and are not
    // added to the free-var set. Order of first appearance is
    // preserved so error messages are predictable.
    //
    // Also used by codegen.hpp to lift `fn(x) -> EXPR` to a static
    // C function plus an env struct of captured variables.
    static void collect_free_vars(const Expr& e,
                                  std::vector<std::string>& bound,
                                  std::vector<std::string>& out) {
        switch (e.kind) {
            case ExprKind::Ident: {
                const std::string& name = e.str;
                if (is_known_fn_name(name)) return;
                // Bound by an enclosing FnExpr?
                for (const auto& b : bound) if (b == name) return;
                // Already collected?
                for (const auto& f : out) if (f == name) return;
                out.push_back(name);
                return;
            }
            case ExprKind::Unary:
                if (e.rhs) collect_free_vars(*e.rhs, bound, out);
                return;
            case ExprKind::Binary:
                if (e.lhs) collect_free_vars(*e.lhs, bound, out);
                if (e.rhs) collect_free_vars(*e.rhs, bound, out);
                return;
            case ExprKind::Index:
                if (e.callee) collect_free_vars(*e.callee, bound, out);
                for (const auto& el : e.elems)
                    if (el) collect_free_vars(*el, bound, out);
                return;
            case ExprKind::Slice:
                if (e.slice_lo) collect_free_vars(*e.slice_lo, bound, out);
                if (e.slice_hi) collect_free_vars(*e.slice_hi, bound, out);
                return;
            case ExprKind::Call:
                if (e.callee) collect_free_vars(*e.callee, bound, out);
                for (const auto& a : e.elems)
                    if (a) collect_free_vars(*a, bound, out);
                return;
            case ExprKind::VecLit:
                for (const auto& el : e.elems)
                    if (el) collect_free_vars(*el, bound, out);
                return;
            case ExprKind::MatLit:
                for (const auto& row : e.rows)
                    for (const auto& el : row)
                        if (el) collect_free_vars(*el, bound, out);
                return;
            case ExprKind::FnExpr: {
                // Params shadow names from the outer scope; push them
                // onto `bound`, walk the body, then pop.
                size_t before = bound.size();
                for (const auto& p : e.params) bound.push_back(p);
                if (e.lhs) collect_free_vars(*e.lhs, bound, out);
                bound.resize(before);
                return;
            }
            case ExprKind::NumberLit:
            case ExprKind::StringLit:
            case ExprKind::BoolLit:
            case ExprKind::NilLit:
                return;
        }
    }

private:
    // Wrap `body` as `fn(var) -> body`. Used by parse_phrase for
    // closure-shaped holes after picking the parameter via free-var
    // analysis.
    static ExprPtr wrap_fn_expr(const std::string& var, ExprPtr body) {
        Span s = body->span;
        auto e = std::make_unique<Expr>(ExprKind::FnExpr, s);
        e->params.push_back(var);
        e->lhs = std::move(body);
        return e;
    }

    // ---- Anonymous functions (`fn(x) -> EXPR`) ---------------------
    //
    // Disambiguates `fn` (an Ident) from `fn(x) -> EXPR` (an anonymous
    // function). The pattern we accept is `fn ( IDENT (, IDENT)* ) ->`;
    // anything else leaves the input alone so `fn` keeps working as a
    // regular variable / function name.
    //
    // Caller has already verified the current token is `fn` and the
    // next is `(`. We peek forward to confirm the rest of the header
    // matches, without consuming any tokens.
    bool looks_like_fn_expr_header(size_t fn_pos) const {
        // Walk: fn ( IDENT (, IDENT)* ) ->
        size_t p = fn_pos + 1; // points at '('
        if (p >= toks.size() || toks[p].kind != Tok::LParen) return false;
        ++p;
        // At least one parameter ident, or an empty parameter list.
        if (p < toks.size() && toks[p].kind == Tok::Ident) {
            ++p;
            while (p < toks.size() && toks[p].kind == Tok::Comma) {
                ++p;
                if (p >= toks.size() || toks[p].kind != Tok::Ident) return false;
                ++p;
            }
        }
        if (p >= toks.size() || toks[p].kind != Tok::RParen) return false;
        ++p;
        return p < toks.size() && toks[p].kind == Tok::Arrow;
    }

    // Parse `fn(x[, y, ...]) -> EXPR`. Current token must be `fn`.
    ExprPtr parse_fn_expr() {
        Span start = cur().span;
        ++pos; // 'fn'
        expect(Tok::LParen, "'(' in fn(...) -> ...");
        std::vector<std::string> params;
        if (!check(Tok::RParen)) {
            const Token& first = expect(Tok::Ident,
                "parameter name in fn(...) -> ...");
            params.push_back(first.text);
            while (match(Tok::Comma)) {
                const Token& nxt = expect(Tok::Ident,
                    "parameter name in fn(...) -> ...");
                params.push_back(nxt.text);
            }
        }
        expect(Tok::RParen, "')' closing fn(...) parameter list");
        expect(Tok::Arrow, "'->' before fn(...) body");
        ExprPtr body = parse_expr();

        auto e = std::make_unique<Expr>(ExprKind::FnExpr,
            Span::merge(start, body->span));
        e->params = std::move(params);
        e->lhs = std::move(body);  // body lives in `lhs` slot
        return e;
    }

    // ---- Plain-speak phrases (`take ...`) ---------------------------
    //
    // Called from parse_primary when we've decided `take` should
    // introduce a phrase. The current token is `take` (an Ident);
    // we skip it, gobble tokens until end-of-phrase, match against
    // src/phrases.hpp::phrase_table(), and emit a plain Call.
    //
    // End-of-phrase is the first newline, semicolon, EOF, or any
    // unmatched closing brace/bracket/paren -- i.e. the same
    // terminators that would naturally end a top-level expression.
    // An open brace also ends the phrase, so phrases work inside
    // `for i in take the range from 0 to 10 { ... }`.
    ExprPtr parse_phrase() {
        Span start = cur().span;
        ++pos; // 'take'

        // Collect the phrase's tokens, tracking bracket depth so a
        // parenthesized sub-expression inside the phrase doesn't end
        // it early on its outer ')'. At depth 0 the phrase ends on
        // any token that would naturally separate or close the
        // surrounding context: statement terminators, block braces,
        // a comma (so phrase-as-call-arg works), or an outer closing
        // paren/bracket (so phrase-as-call-arg / phrase-in-vec work).
        size_t phrase_start = pos;
        int depth = 0;
        while (pos < toks.size()) {
            Tok k = toks[pos].kind;
            if (depth == 0) {
                if (k == Tok::Newline || k == Tok::Semicolon
                 || k == Tok::Eof || k == Tok::RBrace
                 || k == Tok::LBrace || k == Tok::Comma
                 || k == Tok::RParen || k == Tok::RBracket) {
                    break;
                }
            }
            if (k == Tok::LParen || k == Tok::LBracket) ++depth;
            else if (k == Tok::RParen || k == Tok::RBracket) {
                if (depth > 0) --depth;
            }
            ++pos;
        }

        std::vector<Token> phrase_toks(toks.begin() + phrase_start,
                                       toks.begin() + pos);

        if (phrase_toks.empty()) {
            throw Diag(start, "`take` must be followed by a phrase like "
                              "`take the sum of v`");
        }

        // Try each pattern in declared order; first match wins.
        const auto& table = phrase_table();
        for (const auto& pat : table) {
            std::vector<std::pair<size_t, size_t>> slices;
            if (!try_match_phrase(pat, phrase_toks, slices)) continue;

            // Build the Call expression. The function reference is
            // an Ident named after the pattern's `function` field --
            // resolved at runtime via normal lexical lookup, so the
            // user can shadow / override these names if they want.
            Span end_span = phrase_toks.back().span;
            auto callee = std::make_unique<Expr>(ExprKind::Ident, start);
            callee->str = pat.function;

            std::vector<ExprPtr> hole_exprs;
            hole_exprs.reserve(slices.size());
            for (size_t hi_idx = 0; hi_idx < slices.size(); ++hi_idx) {
                auto [lo, hi] = slices[hi_idx];
                // Re-parse the hole's token slice as a knot
                // expression. Need to terminate it with an Eof so the
                // sub-parser knows when to stop.
                std::vector<Token> sub(phrase_toks.begin() + lo,
                                       phrase_toks.begin() + hi);
                Token eof_tok;
                eof_tok.kind = Tok::Eof;
                eof_tok.span = phrase_toks[hi - 1].span;
                sub.push_back(eof_tok);
                Parser sub_parser(sub);
                ExprPtr hole = sub_parser.parse_expr();

                // Closure-shaped hole? Walk the parsed expression
                // for free variables, require exactly one, wrap as
                // fn(VAR) -> HOLE.
                bool is_closure = false;
                for (int idx : pat.closure_holes) {
                    if ((size_t)idx == hi_idx) { is_closure = true; break; }
                }
                if (is_closure) {
                    std::vector<std::string> bound, frees;
                    collect_free_vars(*hole, bound, frees);
                    if (frees.size() == 1) {
                        hole = wrap_fn_expr(frees[0], std::move(hole));
                    } else if (frees.empty()) {
                        throw Diag(start,
                            "`take` closure-phrase: no free variable found "
                            "in the expression body -- the integrand/equation "
                            "must depend on at least one unbound name");
                    } else {
                        std::string names;
                        for (size_t i = 0; i < frees.size(); ++i) {
                            if (i) names += ", ";
                            names += frees[i];
                        }
                        throw Diag(start,
                            "`take` closure-phrase: multiple candidate "
                            "free variables (" + names + ") -- knot doesn't "
                            "know which to use as the parameter. Rewrite "
                            "using `fn(x) -> ...` and pass directly to the "
                            "underlying function instead");
                    }
                }
                hole_exprs.push_back(std::move(hole));
            }

            auto call = std::make_unique<Expr>(ExprKind::Call,
                Span::merge(start, end_span));
            call->callee = std::move(callee);
            call->elems = std::move(hole_exprs);

            // Translation hint: show the user what their English
            // phrase desugared into. Off when phrase_hints_enabled()
            // is false (set by the CLI's --no-hints flag).
            // Emitted at parse time so the hints appear before the
            // program's stdout. Source-slice the phrase and each
            // hole directly so operators / punctuation come through
            // exactly as the user typed them.
            if (phrase_hints_enabled() && source != nullptr) {
                auto src_slice = [&](Span sp) -> std::string {
                    if (sp.start + sp.length > source->size()) return "";
                    return source->substr(sp.start, sp.length);
                };

                // Full phrase: from `take` to the last phrase token.
                Span full;
                full.start = start.start;
                full.length =
                    (phrase_toks.back().span.start
                     + phrase_toks.back().span.length)
                    - start.start;
                std::string typed = src_slice(full);

                std::vector<std::string> hole_texts;
                hole_texts.reserve(slices.size());
                for (const auto& [lo, hi] : slices) {
                    Span hsp;
                    hsp.start = phrase_toks[lo].span.start;
                    hsp.length =
                        (phrase_toks[hi - 1].span.start
                         + phrase_toks[hi - 1].span.length)
                        - hsp.start;
                    hole_texts.push_back(src_slice(hsp));
                }

                std::cerr << "# " << typed
                          << "  ->  "
                          << format_canonical(pat.canonical, hole_texts)
                          << "\n";
            }

            return call;
        }

        // No pattern matched. Build a snippet of the source phrase
        // text so the diagnostic is actionable.
        std::string snippet;
        for (const auto& t : phrase_toks) {
            if (!snippet.empty()) snippet += " ";
            snippet += t.text.empty()
                ? std::string(tok_name(t.kind))
                : t.text;
        }
        throw Diag(start,
            "no `take` phrase matches: `take " + snippet + "`");
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
                // `take` is a soft keyword: phrase introducer if the
                // next token is an Ident or Number (matching one of
                // the patterns in src/phrases.hpp), otherwise a
                // normal identifier so `take = 42`, `take(args)`, and
                // `take + 1` all still work.
                if (t.text == "take" && pos + 1 < toks.size()) {
                    Tok nk = toks[pos + 1].kind;
                    if (nk == Tok::Ident || nk == Tok::Number) {
                        return parse_phrase();
                    }
                }
                // `fn` is a soft keyword: anonymous-function
                // introducer when the next token is `(` and the
                // `(IDENT[, IDENT]*) ->` shape is present. Anything
                // else (including `fn(5)`, `fn`-as-variable) falls
                // back to normal identifier handling.
                if (t.text == "fn" && pos + 1 < toks.size()
                 && toks[pos + 1].kind == Tok::LParen
                 && looks_like_fn_expr_header(pos)) {
                    return parse_fn_expr();
                }
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
