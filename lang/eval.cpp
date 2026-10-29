#include <exception>

#include "../runtime/scheduler.h"
#include "lang.h"

namespace {

ValuePtr make_int(long long n) {
    auto v = std::make_shared<Value>();
    v->kind = ValueKind::Int;
    v->number = n;
    return v;
}

ValuePtr make_pair(ValuePtr a, ValuePtr b) {
    auto v = std::make_shared<Value>();
    v->kind = ValueKind::Pair;
    v->first = std::move(a);
    v->second = std::move(b);
    return v;
}

EnvPtr extend(const EnvPtr& env, int name, ValuePtr value) {
    // A new node in front; the old list is shared, never changed.
    return std::make_shared<const Env>(Env{name, std::move(value), env});
}

[[noreturn]] void fail(int line, const std::string& msg) {
    throw LangError("line " + std::to_string(line) + ": " + msg);
}

long long expect_int(const ValuePtr& v, int line) {
    if (v->kind != ValueKind::Int) fail(line, "expected an integer");
    return v->number;
}

}  // namespace

std::string value_to_string(const ValuePtr& v) {
    switch (v->kind) {
        case ValueKind::Int: return std::to_string(v->number);
        case ValueKind::Pair:
            return "(" + value_to_string(v->first) + ", " + value_to_string(v->second) + ")";
        case ValueKind::Closure: return "<function>";
        case ValueKind::Builtin: return "<builtin>";
    }
    return "?";
}

Evaluator::Evaluator(Scheduler* sched, int par_cutoff)
    : sched_(sched), par_cutoff_(par_cutoff) {}

ValuePtr Evaluator::run(const Program& program) {
    names_ = &program.names;
    tasks_spawned_ = 0;
    EnvPtr env;
    for (int b = 0; b < NumBuiltins; b++) {
        auto v = std::make_shared<Value>();
        v->kind = ValueKind::Builtin;
        v->builtin = b;
        env = extend(env, b, v);
    }
    if (!sched_) return eval(program.root.get(), env, 0);

    // Run the whole program as one task so all evaluation happens on the
    // worker threads; the calling thread just waits.
    ValuePtr result;
    TaskGroup group;
    sched_->spawn(group, [&] { result = eval(program.root.get(), env, 0); });
    sched_->wait(group);
    return result;
}

ValuePtr Evaluator::apply(const ValuePtr& f, const ValuePtr& arg, int par_depth, int line) {
    if (f->kind == ValueKind::Builtin) {
        if (arg->kind != ValueKind::Pair) fail(line, "fst/snd expect a pair");
        return f->builtin == BuiltinFst ? arg->first : arg->second;
    }
    if (f->kind != ValueKind::Closure) fail(line, "trying to call something that is not a function");
    EnvPtr env = f->env;
    if (f->self_name >= 0) env = extend(env, f->self_name, f);   // lets `let rec` call itself
    env = extend(env, f->param, arg);
    return eval(f->body, env, par_depth);
}

ValuePtr Evaluator::eval(const Expr* e, const EnvPtr& env, int par_depth) {
    switch (e->kind) {
        case ExprKind::Int:
            return make_int(e->number);

        case ExprKind::Var:
            for (const Env* p = env.get(); p; p = p->next.get())
                if (p->name == e->name) return p->value;
            fail(e->line, "unknown name '" + (*names_)[e->name] + "'");

        case ExprKind::Neg:
            return make_int(-expect_int(eval(e->a.get(), env, par_depth), e->line));

        case ExprKind::BinOp: {
            long long x = expect_int(eval(e->a.get(), env, par_depth), e->line);
            long long y = expect_int(eval(e->b.get(), env, par_depth), e->line);
            switch (e->op) {
                case Tok::Plus: return make_int(x + y);
                case Tok::Minus: return make_int(x - y);
                case Tok::Star: return make_int(x * y);
                case Tok::Slash:
                    if (y == 0) fail(e->line, "division by zero");
                    return make_int(x / y);
                case Tok::Percent:
                    if (y == 0) fail(e->line, "division by zero");
                    return make_int(x % y);
                case Tok::Less: return make_int(x < y);
                case Tok::LessEq: return make_int(x <= y);
                case Tok::Greater: return make_int(x > y);
                case Tok::GreaterEq: return make_int(x >= y);
                case Tok::EqEq: return make_int(x == y);
                case Tok::NotEq: return make_int(x != y);
                default: fail(e->line, "bad operator");
            }
        }

        case ExprKind::If:
            if (expect_int(eval(e->a.get(), env, par_depth), e->line) != 0)
                return eval(e->b.get(), env, par_depth);
            return eval(e->c.get(), env, par_depth);

        case ExprKind::Let:
            return eval(e->b.get(), extend(env, e->name, eval(e->a.get(), env, par_depth)),
                        par_depth);

        case ExprKind::LetPair: {
            ValuePtr v = eval(e->a.get(), env, par_depth);
            if (v->kind != ValueKind::Pair) fail(e->line, "let (a, b) needs a pair");
            EnvPtr env2 = extend(extend(env, e->name, v->first), e->name2, v->second);
            return eval(e->b.get(), env2, par_depth);
        }

        case ExprKind::LetRec: {
            auto f = std::make_shared<Value>();
            f->kind = ValueKind::Closure;
            f->body = e->a.get();
            f->param = e->name2;
            f->self_name = e->name;
            f->env = env;   // not including f itself, so no reference cycle
            return eval(e->b.get(), extend(env, e->name, f), par_depth);
        }

        case ExprKind::Fun: {
            auto f = std::make_shared<Value>();
            f->kind = ValueKind::Closure;
            f->body = e->a.get();
            f->param = e->name;
            f->env = env;
            return f;
        }

        case ExprKind::App: {
            ValuePtr f = eval(e->a.get(), env, par_depth);
            ValuePtr arg = eval(e->b.get(), env, par_depth);
            return apply(f, arg, par_depth, e->line);
        }

        case ExprKind::Pair:
            return make_pair(eval(e->a.get(), env, par_depth), eval(e->b.get(), env, par_depth));

        case ExprKind::Par: {
            if (!sched_ || par_depth >= par_cutoff_) {
                // Too deep (or no scheduler): same result, no task.
                return make_pair(eval(e->a.get(), env, par_depth + 1),
                                 eval(e->b.get(), env, par_depth + 1));
            }
            // Left side becomes a task, right side runs here, then wait.
            // Both read the same immutable env; each writes only its own slot.
            ValuePtr left;
            TaskGroup group;
            tasks_spawned_++;
            sched_->spawn(group, [&] { left = eval(e->a.get(), env, par_depth + 1); });
            ValuePtr right;
            try {
                right = eval(e->b.get(), env, par_depth + 1);
            } catch (...) {
                // Must not leave while the task still uses our locals.
                try { sched_->wait(group); } catch (...) {}
                throw;
            }
            sched_->wait(group);   // rethrows if the left side failed
            return make_pair(left, right);
        }
    }
    fail(e->line, "unknown expression");
}
