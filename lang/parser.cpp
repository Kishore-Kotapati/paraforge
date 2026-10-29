// Recursive descent parser. One function per precedence level:
//   expr -> compare -> sum -> term -> unary -> app -> atom
#include <unordered_map>

#include "lang.h"

namespace {

class Parser {
public:
    explicit Parser(const std::string& source) : tokens_(tokenize(source)) {
        intern("fst");   // must match BuiltinFst
        intern("snd");   // must match BuiltinSnd
    }

    Program parse() {
        Program p;
        p.root = parse_expr();
        expect(Tok::End, "end of input");
        p.names = names_;
        return p;
    }

private:
    const Token& peek() const { return tokens_[pos_]; }
    const Token& advance() { return tokens_[pos_++]; }
    bool accept(Tok kind) {
        if (peek().kind != kind) return false;
        pos_++;
        return true;
    }
    const Token& expect(Tok kind, const char* what) {
        if (peek().kind != kind) error(std::string("expected ") + what);
        return advance();
    }
    [[noreturn]] void error(const std::string& msg) const {
        throw LangError("line " + std::to_string(peek().line) + ": " + msg +
                        ", found '" + peek().text + "'");
    }

    int intern(const std::string& name) {
        auto it = ids_.find(name);
        if (it != ids_.end()) return it->second;
        int id = (int)names_.size();
        names_.push_back(name);
        ids_[name] = id;
        return id;
    }

    std::unique_ptr<Expr> node(ExprKind kind) {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->line = peek().line;
        return e;
    }

    // Builds fun p1 -> fun p2 -> ... -> body
    std::unique_ptr<Expr> wrap_params(const std::vector<int>& params, size_t from,
                                      std::unique_ptr<Expr> body) {
        for (size_t i = params.size(); i-- > from;) {
            auto f = node(ExprKind::Fun);
            f->name = params[i];
            f->a = std::move(body);
            body = std::move(f);
        }
        return body;
    }

    std::unique_ptr<Expr> parse_expr() {
        if (accept(Tok::Let)) return parse_let();
        if (accept(Tok::If)) {
            auto e = node(ExprKind::If);
            e->a = parse_expr();
            expect(Tok::Then, "'then'");
            e->b = parse_expr();
            expect(Tok::Else, "'else'");
            e->c = parse_expr();
            return e;
        }
        if (accept(Tok::Fun)) {
            std::vector<int> params;
            while (peek().kind == Tok::Ident) params.push_back(intern(advance().text));
            if (params.empty()) error("expected a parameter name after 'fun'");
            expect(Tok::Arrow, "'->'");
            return wrap_params(params, 0, parse_expr());
        }
        return parse_compare();
    }

    std::unique_ptr<Expr> parse_let() {
        // let (a, b) = e in body
        if (accept(Tok::LParen)) {
            auto e = node(ExprKind::LetPair);
            e->name = intern(expect(Tok::Ident, "a name").text);
            expect(Tok::Comma, "','");
            e->name2 = intern(expect(Tok::Ident, "a name").text);
            expect(Tok::RParen, "')'");
            expect(Tok::Assign, "'='");
            e->a = parse_expr();
            expect(Tok::In, "'in'");
            e->b = parse_expr();
            return e;
        }

        bool is_rec = accept(Tok::Rec);
        int name = intern(expect(Tok::Ident, "a name").text);
        std::vector<int> params;
        while (peek().kind == Tok::Ident) params.push_back(intern(advance().text));
        expect(Tok::Assign, "'='");
        auto value = parse_expr();
        expect(Tok::In, "'in'");

        if (is_rec) {
            if (params.empty()) error("'let rec' needs at least one parameter");
            // let rec f a b = v  ->  LetRec(f, param a, body = fun b -> v)
            auto e = node(ExprKind::LetRec);
            e->name = name;
            e->name2 = params[0];
            e->a = wrap_params(params, 1, std::move(value));
            e->b = parse_expr();
            return e;
        }
        auto e = node(ExprKind::Let);
        e->name = name;
        e->a = wrap_params(params, 0, std::move(value));
        e->b = parse_expr();
        return e;
    }

    std::unique_ptr<Expr> binary(Tok op, std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs) {
        auto e = node(ExprKind::BinOp);
        e->op = op;
        e->a = std::move(lhs);
        e->b = std::move(rhs);
        return e;
    }

    std::unique_ptr<Expr> parse_compare() {
        auto lhs = parse_sum();
        Tok k = peek().kind;
        if (k == Tok::Less || k == Tok::LessEq || k == Tok::Greater ||
            k == Tok::GreaterEq || k == Tok::EqEq || k == Tok::NotEq) {
            advance();
            return binary(k, std::move(lhs), parse_sum());
        }
        return lhs;
    }

    std::unique_ptr<Expr> parse_sum() {
        auto lhs = parse_term();
        while (peek().kind == Tok::Plus || peek().kind == Tok::Minus) {
            Tok op = advance().kind;
            lhs = binary(op, std::move(lhs), parse_term());
        }
        return lhs;
    }

    std::unique_ptr<Expr> parse_term() {
        auto lhs = parse_unary();
        while (peek().kind == Tok::Star || peek().kind == Tok::Slash ||
               peek().kind == Tok::Percent) {
            Tok op = advance().kind;
            lhs = binary(op, std::move(lhs), parse_unary());
        }
        return lhs;
    }

    std::unique_ptr<Expr> parse_unary() {
        if (accept(Tok::Minus)) {
            auto e = node(ExprKind::Neg);
            e->a = parse_unary();
            return e;
        }
        return parse_app();
    }

    bool starts_atom() const {
        Tok k = peek().kind;
        return k == Tok::Int || k == Tok::Ident || k == Tok::LParen || k == Tok::Par;
    }

    // Function application is just two things side by side: f x y
    std::unique_ptr<Expr> parse_app() {
        auto fn = parse_atom();
        while (starts_atom()) {
            auto e = node(ExprKind::App);
            e->a = std::move(fn);
            e->b = parse_atom();
            fn = std::move(e);
        }
        return fn;
    }

    std::unique_ptr<Expr> parse_atom() {
        if (peek().kind == Tok::Int) {
            auto e = node(ExprKind::Int);
            e->number = advance().number;
            return e;
        }
        if (peek().kind == Tok::Ident) {
            auto e = node(ExprKind::Var);
            e->name = intern(advance().text);
            return e;
        }
        if (accept(Tok::Par)) {
            auto e = node(ExprKind::Par);
            expect(Tok::LParen, "'(' after par");
            e->a = parse_expr();
            expect(Tok::Comma, "',' in par");
            e->b = parse_expr();
            expect(Tok::RParen, "')'");
            return e;
        }
        if (accept(Tok::LParen)) {
            auto first = parse_expr();
            if (accept(Tok::Comma)) {
                auto e = node(ExprKind::Pair);
                e->a = std::move(first);
                e->b = parse_expr();
                expect(Tok::RParen, "')'");
                return e;
            }
            expect(Tok::RParen, "')'");
            return first;
        }
        error("expected an expression");
    }

    std::vector<Token> tokens_;
    size_t pos_ = 0;
    std::vector<std::string> names_;
    std::unordered_map<std::string, int> ids_;
};

}  // namespace

Program parse_program(const std::string& source) {
    return Parser(source).parse();
}
