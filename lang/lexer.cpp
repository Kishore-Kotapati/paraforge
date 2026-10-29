#include <cctype>

#include "lang.h"

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> out;
    int line = 1;
    size_t i = 0;

    auto add = [&](Tok kind, std::string text) {
        out.push_back({kind, std::move(text), 0, line});
    };

    while (i < src.size()) {
        char ch = src[i];
        if (ch == '\n') {
            line++;
            i++;
        } else if (std::isspace((unsigned char)ch)) {
            i++;
        } else if (ch == '#') {   // comment to end of line
            while (i < src.size() && src[i] != '\n') i++;
        } else if (std::isdigit((unsigned char)ch)) {
            size_t start = i;
            while (i < src.size() && std::isdigit((unsigned char)src[i])) i++;
            Token t{Tok::Int, src.substr(start, i - start), 0, line};
            try {
                t.number = std::stoll(t.text);
            } catch (...) {
                throw LangError("line " + std::to_string(line) + ": number too large");
            }
            out.push_back(t);
        } else if (std::isalpha((unsigned char)ch) || ch == '_') {
            size_t start = i;
            while (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i] == '_'))
                i++;
            std::string word = src.substr(start, i - start);
            Tok kind = Tok::Ident;
            if (word == "let") kind = Tok::Let;
            else if (word == "rec") kind = Tok::Rec;
            else if (word == "in") kind = Tok::In;
            else if (word == "if") kind = Tok::If;
            else if (word == "then") kind = Tok::Then;
            else if (word == "else") kind = Tok::Else;
            else if (word == "fun") kind = Tok::Fun;
            else if (word == "par") kind = Tok::Par;
            add(kind, word);
        } else {
            // Two-character operators first, so "<=" is not read as "<" "=".
            std::string two = src.substr(i, 2);
            if (two == "->") { add(Tok::Arrow, two); i += 2; continue; }
            if (two == "<=") { add(Tok::LessEq, two); i += 2; continue; }
            if (two == ">=") { add(Tok::GreaterEq, two); i += 2; continue; }
            if (two == "==") { add(Tok::EqEq, two); i += 2; continue; }
            if (two == "!=") { add(Tok::NotEq, two); i += 2; continue; }
            Tok kind;
            switch (ch) {
                case '(': kind = Tok::LParen; break;
                case ')': kind = Tok::RParen; break;
                case ',': kind = Tok::Comma; break;
                case '=': kind = Tok::Assign; break;
                case '+': kind = Tok::Plus; break;
                case '-': kind = Tok::Minus; break;
                case '*': kind = Tok::Star; break;
                case '/': kind = Tok::Slash; break;
                case '%': kind = Tok::Percent; break;
                case '<': kind = Tok::Less; break;
                case '>': kind = Tok::Greater; break;
                default:
                    throw LangError("line " + std::to_string(line) +
                                    ": unexpected character '" + std::string(1, ch) + "'");
            }
            add(kind, std::string(1, ch));
            i++;
        }
    }
    add(Tok::End, "end of input");
    return out;
}
