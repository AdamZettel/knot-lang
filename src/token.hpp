#pragma once
#include "span.hpp"
#include <string>

namespace knot {

enum class Tok {
    // Literals
    Number, String, Ident,
    // Keywords
    If, Else, While, Loop, As, For, To, In, Def, Return, True, False, None_,
    // Punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, Newline,
    // Operators
    Plus, Minus, Star, Slash, Percent, At,
    Eq, PlusEq, MinusEq, StarEq, SlashEq,
    EqEq, BangEq, Lt, LtEq, Gt, GtEq,
    Bang, AndAnd, OrOr,
    // Special
    Eof,
};

struct Token {
    Tok kind;
    std::string text;   // source slice (for idents, numbers, strings)
    double number = 0;  // parsed value for Tok::Number
    Span span;
};

inline const char* tok_name(Tok t) {
    switch (t) {
        case Tok::Number: return "number";
        case Tok::String: return "string";
        case Tok::Ident: return "identifier";
        case Tok::If: return "'if'";
        case Tok::Else: return "'else'";
        case Tok::While: return "'while'";
        case Tok::Loop: return "'loop'";
        case Tok::As: return "'as'";
        case Tok::For: return "'for'";
        case Tok::To: return "'to'";
        case Tok::In: return "'in'";
        case Tok::Def: return "'def'";
        case Tok::Return: return "'return'";
        case Tok::True: return "'True'";
        case Tok::False: return "'False'";
        case Tok::None_: return "'None'";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::LBracket: return "'['";
        case Tok::RBracket: return "']'";
        case Tok::Comma: return "','";
        case Tok::Semicolon: return "';'";
        case Tok::Colon: return "':'";
        case Tok::Newline: return "newline";
        case Tok::Plus: return "'+'";
        case Tok::Minus: return "'-'";
        case Tok::Star: return "'*'";
        case Tok::Slash: return "'/'";
        case Tok::Percent: return "'%'";
        case Tok::At: return "'@'";
        case Tok::Eq: return "'='";
        case Tok::PlusEq: return "'+='";
        case Tok::MinusEq: return "'-='";
        case Tok::StarEq: return "'*='";
        case Tok::SlashEq: return "'/='";
        case Tok::EqEq: return "'=='";
        case Tok::BangEq: return "'!='";
        case Tok::Lt: return "'<'";
        case Tok::LtEq: return "'<='";
        case Tok::Gt: return "'>'";
        case Tok::GtEq: return "'>='";
        case Tok::Bang: return "'!'";
        case Tok::AndAnd: return "'&&'";
        case Tok::OrOr: return "'||'";
        case Tok::Eof: return "end of input";
    }
    return "?";
}

} // namespace knot
