// PFL: a tiny functional language whose `par(a, b)` runs on the ParaForge runtime.
//
//   let rec fib n =
//     if n < 2 then n
//     else let (a, b) = par(fib (n - 1), fib (n - 2)) in a + b
//   in fib 25
//
// Values never change after they are created (no assignment in the language),
// and environments are immutable linked lists. That is why two par branches
// can share the same environment without any locks.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class Scheduler;

struct LangError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------- Tokens ----------

enum class Tok {
    Int, Ident, Let, Rec, In, If, Then, Else, Fun, Par,
    LParen, RParen, Comma, Arrow, Assign,
    Plus, Minus, Star, Slash, Percent,
    Less, LessEq, Greater, GreaterEq, EqEq, NotEq, End
};

struct Token {
    Tok kind;
    std::string text;
    long long number = 0;
    int line = 1;
};

std::vector<Token> tokenize(const std::string& source);

// ---------- Syntax tree ----------

enum class ExprKind { Int, Var, BinOp, Neg, If, Let, LetPair, LetRec, Fun, App, Pair, Par };

struct Expr {
    ExprKind kind;
    long long number = 0;     // Int
    Tok op = Tok::Plus;       // BinOp
    int name = -1;            // Var / Let / LetRec name, Fun parameter
    int name2 = -1;           // LetPair second name
    std::unique_ptr<Expr> a, b, c;   // children (meaning depends on kind)
    int line = 1;
};

struct Program {
    std::unique_ptr<Expr> root;
    std::vector<std::string> names;   // identifier id -> text
};

// Names that exist before the program starts (builtins).
enum Builtin { BuiltinFst = 0, BuiltinSnd = 1, NumBuiltins = 2 };

Program parse_program(const std::string& source);

// ---------- Values and environments ----------

struct Value;
struct Env;
using ValuePtr = std::shared_ptr<const Value>;
using EnvPtr = std::shared_ptr<const Env>;

enum class ValueKind { Int, Pair, Closure, Builtin };

struct Value {
    ValueKind kind;
    long long number = 0;          // Int
    ValuePtr first, second;        // Pair
    const Expr* body = nullptr;    // Closure
    int param = -1;                // Closure
    int self_name = -1;            // Closure made by `let rec`: name bound to itself
    EnvPtr env;                    // Closure: captured environment
    int builtin = -1;              // Builtin
};

struct Env {
    int name;
    ValuePtr value;
    EnvPtr next;
};

std::string value_to_string(const ValuePtr& v);

// ---------- Evaluator ----------

class Evaluator {
public:
    // sched == nullptr: par runs both sides one after the other.
    // par_cutoff: only the first `par_cutoff` levels of nested par spawn a
    // task; deeper ones run inline, so tasks do not get too small.
    Evaluator(Scheduler* sched, int par_cutoff);

    ValuePtr run(const Program& program);
    int64_t tasks_spawned() const { return tasks_spawned_; }

private:
    ValuePtr eval(const Expr* e, const EnvPtr& env, int par_depth);
    ValuePtr apply(const ValuePtr& f, const ValuePtr& arg, int par_depth, int line);
    const std::vector<std::string>* names_ = nullptr;

    Scheduler* sched_;
    int par_cutoff_;
    std::atomic<int64_t> tasks_spawned_{0};
};
