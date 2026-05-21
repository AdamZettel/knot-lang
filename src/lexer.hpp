#pragma once
#include "diag.hpp"
#include "token.hpp"
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace knot {

class Lexer {
    const std::string& src;
    size_t pos = 0;
    int line = 1, col = 1;
    // Bracket depth: while > 0, newlines are whitespace, not statement
    // terminators. Standard implicit line continuation, as in Python/Go.
    int bracket_depth = 0;

public:
    explicit Lexer(const std::string& s) : src(s) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        bool last_was_terminator = true; // suppress leading/duplicate newlines
        while (true) {
            skip_inline_ws_and_comments();
            if (pos >= src.size()) {
                Token t; t.kind = Tok::Eof; t.span = here(0);
                out.push_back(t);
                break;
            }

            char c = peek();
            if (c == '\n') {
                Span s = here(1);
                advance();
                if (bracket_depth == 0 && !last_was_terminator) {
                    Token t; t.kind = Tok::Newline; t.span = s;
                    out.push_back(t);
                    last_was_terminator = true;
                }
                continue;
            }

            Token t = next_token();
            // Track grouping depth so newlines inside (...) and [...] are
            // whitespace. Braces are statement-block delimiters, NOT
            // expression groupings, so newlines inside { } must remain real
            // statement terminators.
            switch (t.kind) {
                case Tok::LParen: case Tok::LBracket:
                    ++bracket_depth; break;
                case Tok::RParen: case Tok::RBracket:
                    if (bracket_depth > 0) --bracket_depth;
                    break;
                default: break;
            }
            out.push_back(t);
            // After `;`, `{`, or `}`, squash any immediately-following
            // newline since it would be a redundant terminator.
            last_was_terminator = (t.kind == Tok::Semicolon
                                || t.kind == Tok::LBrace
                                || t.kind == Tok::RBrace);
        }
        return out;
    }

private:
    char peek(size_t off = 0) const {
        return pos + off < src.size() ? src[pos + off] : '\0';
    }

    void advance() {
        if (pos >= src.size()) return;
        if (src[pos] == '\n') { ++line; col = 1; }
        else { ++col; }
        ++pos;
    }

    Span here(size_t len) const {
        Span s; s.start = pos; s.length = len; s.line = line; s.col = col;
        return s;
    }

    void skip_inline_ws_and_comments() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\r') advance();
            else if (c == '#') {
                while (pos < src.size() && src[pos] != '\n') advance();
            } else if (c == '\\' && peek(1) == '\n') {
                advance(); advance(); // explicit line continuation
            } else break;
        }
    }

    Token next_token() {
        Span start = here(0);
        char c = peek();

        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t b = pos;
            while (std::isalnum((unsigned char)peek()) || peek() == '_') advance();
            std::string text = src.substr(b, pos - b);
            Token t;
            t.text = text;
            t.span = start;
            t.span.length = pos - start.start;
            static const std::unordered_map<std::string, Tok> kw = {
                {"if", Tok::If}, {"else", Tok::Else},
                {"while", Tok::While}, {"loop", Tok::Loop}, {"as", Tok::As},
                {"for", Tok::For}, {"to", Tok::To}, {"in", Tok::In},
                {"def", Tok::Def}, {"return", Tok::Return},
                {"break", Tok::Break}, {"continue", Tok::Continue},
                {"True", Tok::True}, {"False", Tok::False}, {"None", Tok::None_},
                // Python-style word operators, alongside symbol forms.
                {"and", Tok::AndAnd}, {"or", Tok::OrOr}, {"not", Tok::Bang},
            };
            auto it = kw.find(text);
            t.kind = (it != kw.end()) ? it->second : Tok::Ident;
            return t;
        }

        if (std::isdigit((unsigned char)c)) {
            size_t b = pos;
            while (std::isdigit((unsigned char)peek())) advance();
            if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {
                advance();
                while (std::isdigit((unsigned char)peek())) advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                advance();
                if (peek() == '+' || peek() == '-') advance();
                while (std::isdigit((unsigned char)peek())) advance();
            }
            std::string text = src.substr(b, pos - b);
            Token t;
            t.kind = Tok::Number;
            t.text = text;
            t.number = std::stod(text);
            t.span = start;
            t.span.length = pos - start.start;
            return t;
        }

        if (c == '"') {
            advance();
            std::string out;
            while (pos < src.size() && peek() != '"') {
                char ch = peek();
                if (ch == '\\') {
                    advance();
                    char e = peek();
                    switch (e) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case '"': out += '"';  break;
                        case '\\': out += '\\'; break;
                        default: out += e;
                    }
                    advance();
                } else {
                    out += ch;
                    advance();
                }
            }
            if (pos >= src.size()) {
                Span s = start; s.length = pos - start.start;
                throw Diag(s, "unterminated string literal");
            }
            advance();
            Token t;
            t.kind = Tok::String;
            t.text = out;
            t.span = start;
            t.span.length = pos - start.start;
            return t;
        }

        auto one = [&](Tok k) {
            Token t; t.kind = k; t.span = start; t.span.length = 1;
            advance();
            return t;
        };
        auto two = [&](Tok k) {
            Token t; t.kind = k; t.span = start; t.span.length = 2;
            advance(); advance();
            return t;
        };

        switch (c) {
            case '(': return one(Tok::LParen);
            case ')': return one(Tok::RParen);
            case '{': return one(Tok::LBrace);
            case '}': return one(Tok::RBrace);
            case '[': return one(Tok::LBracket);
            case ']': return one(Tok::RBracket);
            case ',': return one(Tok::Comma);
            case ';': return one(Tok::Semicolon);
            case ':': return one(Tok::Colon);
            case '@': return one(Tok::At);
            case '+': return peek(1) == '=' ? two(Tok::PlusEq)  : one(Tok::Plus);
            case '-': return peek(1) == '=' ? two(Tok::MinusEq) : one(Tok::Minus);
            case '*': return peek(1) == '=' ? two(Tok::StarEq)  : one(Tok::Star);
            case '/': return peek(1) == '=' ? two(Tok::SlashEq) : one(Tok::Slash);
            case '%': return one(Tok::Percent);
            case '=': return peek(1) == '=' ? two(Tok::EqEq) : one(Tok::Eq);
            case '!': return peek(1) == '=' ? two(Tok::BangEq) : one(Tok::Bang);
            case '<': return peek(1) == '=' ? two(Tok::LtEq) : one(Tok::Lt);
            case '>': return peek(1) == '=' ? two(Tok::GtEq) : one(Tok::Gt);
            case '&': if (peek(1) == '&') return two(Tok::AndAnd); break;
            case '|': if (peek(1) == '|') return two(Tok::OrOr); break;
        }

        Span s = start; s.length = 1;
        throw Diag(s, std::string("unexpected character '") + c + "'");
    }
};

} // namespace knot
