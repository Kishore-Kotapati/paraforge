// Correctness tests for every part. No test framework: a CHECK macro is enough.
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "../lang/lang.h"
#include "../runtime/scheduler.h"
#include "../workloads/chess.h"
#include "../workloads/graph.h"
#include "../workloads/matrix.h"
#include "../workloads/swarm.h"

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            g_passed++;                                                   \
        } else {                                                          \
            g_failed++;                                                   \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static const Policy kPolicies[] = {Policy::Static, Policy::SharedQueue, Policy::WorkStealing};
static const int kThreads[] = {1, 2, 4};

// ---------- runtime ----------

static long fib_tasks(Scheduler& s, int n) {
    if (n < 2) return n;
    long a = 0;
    TaskGroup g;
    s.spawn(g, [&] { a = fib_tasks(s, n - 1); });
    long b = fib_tasks(s, n - 2);
    s.wait(g);
    return a + b;
}

static void test_runtime() {
    std::printf("runtime\n");
    for (Policy p : kPolicies)
        for (int t : kThreads) {
            Scheduler s(t, p);

            // every index visited exactly once
            std::vector<std::atomic<int>> hits(10007);
            s.parallel_for(0, (int64_t)hits.size(), 13, [&](int64_t lo, int64_t hi) {
                for (int64_t i = lo; i < hi; i++) hits[i]++;
            });
            bool once = true;
            for (auto& h : hits) once = once && h == 1;
            CHECK(once);

            // edge cases: empty range, grain bigger than range, grain 0
            int calls = 0;
            s.parallel_for(5, 5, 1, [&](int64_t, int64_t) { calls++; });
            CHECK(calls == 0);
            std::atomic<int64_t> sum{0};
            s.parallel_for(0, 10, 1000, [&](int64_t lo, int64_t hi) {
                for (int64_t i = lo; i < hi; i++) sum += i;
            });
            CHECK(sum == 45);
            sum = 0;
            s.parallel_for(0, 100, 0, [&](int64_t lo, int64_t hi) {
                for (int64_t i = lo; i < hi; i++) sum += i;
            });
            CHECK(sum == 4950);

            // nested spawn/wait (a parent waits while its children wait)
            CHECK(fib_tasks(s, 18) == 2584);

            // an exception inside a task reaches the waiting thread
            TaskGroup g;
            s.spawn(g, [] { throw std::runtime_error("boom"); });
            bool caught = false;
            try {
                s.wait(g);
            } catch (const std::runtime_error&) {
                caught = true;
            }
            CHECK(caught);

            // trace counted every task (fib(18) spawns fib calls with n >= 2)
            CHECK(s.trace().snapshot().tasks > 0);
        }
}

// ---------- matrix ----------

static void test_matrix() {
    std::printf("matrix\n");
    Matrix a = random_matrix(67, 3), b = random_matrix(67, 4);   // odd size on purpose
    Matrix ref = multiply_seq(a, b);
    CHECK(max_abs_diff(ref, multiply_naive(a, b)) < 1e-12);
    for (Policy p : kPolicies)
        for (int t : kThreads) {
            Scheduler s(t, p);
            CHECK(max_abs_diff(ref, multiply_par(s, a, b, 5)) == 0.0);
            CHECK(max_abs_diff(ref, multiply_adaptive(s, a, b, 32)) == 0.0);
        }
    // 1x1 matrix
    Matrix one(1), two(1);
    one.at(0, 0) = 3;
    two.at(0, 0) = 4;
    Scheduler s(2, Policy::WorkStealing);
    CHECK(multiply_par(s, one, two, 1).at(0, 0) == 12);
}

// ---------- graph ----------

static void test_graph() {
    std::printf("graph\n");
    // tiny hand-made graph: 0-1-2, 3 is disconnected
    Graph g = grid_graph(3, 1);
    g.num_vertices = 4;
    g.offset.push_back(g.offset.back());   // vertex 3 has no edges
    std::vector<int> d = bfs_seq(g, 0);
    CHECK(d == std::vector<int>({0, 1, 2, -1}));

    for (const char* kind : {"random", "skewed", "grid"}) {
        Graph big = make_graph(kind, 20000, 11);
        std::vector<int> ref = bfs_seq(big, 5);
        for (Policy p : kPolicies)
            for (int t : kThreads) {
                Scheduler s(t, p);
                CHECK(bfs_par(s, big, 5, 32).dist == ref);
                CHECK(bfs_racy(s, big, 5, 32).dist == ref);   // distances still right
            }
    }
}

// ---------- chess ----------

static void test_chess() {
    std::printf("chess\n");
    Position start = start_position();
    CHECK(perft(start, 1) == 20);
    CHECK(perft(start, 2) == 400);
    CHECK(perft(start, 3) == 8902);
    CHECK(perft(start, 4) == 197281);

    // back-rank mate in one: Ra8#
    Position mate;
    CHECK(parse_fen("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", mate));
    SearchResult r = search_seq(mate, 3);
    CHECK(move_to_string(r.best) == "a1a8");
    CHECK(r.score == kMateScore - 1);

    // stalemate: black has no legal move but is not in check
    Position stale;
    CHECK(parse_fen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", stale));
    CHECK(legal_moves(stale).empty());
    CHECK(!in_check(stale, -1));
    CHECK(search_seq(stale, 3).score == 0);

    // promotion moves exist and parse
    Position promo;
    CHECK(parse_fen("8/P7/8/8/8/8/8/k6K w - - 0 1", promo));
    Move m;
    CHECK(parse_move(promo, "a7a8n", m) && m.promo == KNIGHT);
    CHECK(parse_move(promo, "a7a8", m) && m.promo == QUEEN);
    CHECK(!parse_move(promo, "a7a6", m));

    CHECK(!parse_fen("bad fen", promo));

    // parallel search returns the same score as sequential
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1",
        "r1bq1rk1/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 0 7",
        "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1",
    };
    for (const char* fen : fens) {
        Position pos;
        parse_fen(fen, pos);
        int ref = search_seq(pos, 4).score;
        for (Policy p : kPolicies)
            for (int t : kThreads) {
                Scheduler s(t, p);
                CHECK(search_par(s, pos, 4).score == ref);
            }
    }
}

// ---------- swarm ----------

static void test_swarm() {
    std::printf("swarm\n");
    SwarmConfig cfg;
    cfg.num_agents = 500;
    SwarmState ref = make_swarm(cfg);
    run_swarm(ref, 30, nullptr, 16);
    uint64_t h = swarm_hash(ref);
    for (Policy p : kPolicies)
        for (int t : kThreads) {
            Scheduler s(t, p);
            SwarmState st = make_swarm(cfg);
            run_swarm(st, 30, &s, 16);
            CHECK(swarm_hash(st) == h);   // deterministic for any schedule
        }
    // agents stay inside the world
    bool inside = true;
    for (const Agent& a : ref.agents)
        inside = inside && a.x >= 0 && a.x < cfg.world && a.y >= 0 && a.y < cfg.world;
    CHECK(inside);
}

// ---------- language ----------

static std::string eval_str(const std::string& src, Scheduler* s = nullptr, int cutoff = 10) {
    Program p = parse_program(src);
    Evaluator e(s, cutoff);
    return value_to_string(e.run(p));
}

static bool throws_lang_error(const std::string& src, Scheduler* s = nullptr) {
    try {
        eval_str(src, s);
    } catch (const LangError&) {
        return true;
    }
    return false;
}

static void test_lang() {
    std::printf("lang\n");
    CHECK(eval_str("1 + 2 * 3") == "7");
    CHECK(eval_str("(1 + 2) * 3") == "9");
    CHECK(eval_str("10 - 3 - 2") == "5");     // left associative
    CHECK(eval_str("-4 + 10 % 3") == "-3");
    CHECK(eval_str("if 3 < 4 then 1 else 2") == "1");
    CHECK(eval_str("let x = 5 in let x = x + 1 in x") == "6");   // shadowing
    CHECK(eval_str("let add a b = a + b in add 2 3") == "5");    // currying
    CHECK(eval_str("let f = fun a b -> a * b in let g = f 6 in g 7") == "42");
    CHECK(eval_str("let rec fact n = if n == 0 then 1 else n * fact (n - 1) in fact 10") ==
          "3628800");
    CHECK(eval_str("let (a, b) = (1, (2, 3)) in snd b") == "3");
    CHECK(eval_str("fst (par(1 + 1, 2 + 2))") == "2");
    // closures keep the value they captured
    CHECK(eval_str("let x = 1 in let f y = x + y in let x = 100 in f 1") == "2");

    std::string fib = "let rec fib n = if n < 2 then n else "
                      "let (a, b) = par(fib (n - 1), fib (n - 2)) in a + b in fib 18";
    for (Policy p : kPolicies)
        for (int t : kThreads) {
            Scheduler s(t, p);
            CHECK(eval_str(fib, &s, 8) == "2584");
            CHECK(eval_str(fib, &s, 0) == "2584");     // cutoff 0: never spawns
            CHECK(throws_lang_error("let (a, b) = par(1 / 0, 2) in a", &s));
            CHECK(throws_lang_error("let (a, b) = par(1, 2 / 0) in a", &s));
        }

    CHECK(throws_lang_error("1 +"));              // parse error
    CHECK(throws_lang_error("let x = 1 x"));       // missing 'in'
    CHECK(throws_lang_error("y + 1"));             // unknown name
    CHECK(throws_lang_error("1 2"));               // calling a number
    CHECK(throws_lang_error("1 @ 2"));             // bad character
    CHECK(throws_lang_error("fst 3"));             // fst needs a pair
}

int main() {
    test_runtime();
    test_matrix();
    test_graph();
    test_chess();
    test_swarm();
    test_lang();
    std::printf("\n%d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
