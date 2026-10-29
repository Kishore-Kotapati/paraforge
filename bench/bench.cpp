#include "bench.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>

#include "../lang/lang.h"
#include "../workloads/chess.h"
#include "../workloads/graph.h"
#include "../workloads/matrix.h"
#include "../workloads/swarm.h"

namespace {

// One benchmark: a sequential baseline and a parallel version that reports
// whether its answer matched.
struct Case {
    std::string name;
    std::function<void()> seq;
    std::function<bool(Scheduler&)> par;
    std::function<std::string()> extra;   // details of the last parallel run
    std::vector<Policy> only_policies;    // empty = use the options
};

double time_ms(const std::function<void()>& fn) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

class Reporter {
public:
    explicit Reporter(const std::string& csv_path) {
        if (csv_path.empty()) return;
        bool is_new = !std::ifstream(csv_path).good();
        csv_.open(csv_path, std::ios::app);
        if (is_new)
            csv_ << "workload,policy,threads,time_ms,speedup,efficiency,tasks,steals,"
                    "idle_pct,imbalance,max_depth,correct,seq_ms,extra\n";
    }

    void header() {
        std::printf("%-20s %-10s %3s %10s %8s %6s %9s %8s %6s %6s %5s %3s  %s\n", "workload",
                    "policy", "thr", "time_ms", "speedup", "eff", "tasks", "steals", "idle%",
                    "imbal", "depth", "ok", "extra");
    }

    void row(const std::string& workload, const std::string& policy, int threads,
             double ms, double seq_ms, const RunStats* st, bool ok, const std::string& extra) {
        double speedup = ms > 0 ? seq_ms / ms : 0;
        double eff = speedup / threads;
        long long tasks = st ? (long long)st->tasks : 0;
        long long steals = st ? (long long)st->steals : 0;
        double idle = st ? st->idle_fraction * 100 : 0;
        double imbal = st ? st->imbalance : 0;
        long long depth = st ? (long long)st->max_depth : 0;
        std::printf("%-20s %-10s %3d %10.2f %8.2f %6.2f %9lld %8lld %6.1f %6.2f %5lld %3s  %s\n",
                    workload.c_str(), policy.c_str(), threads, ms, speedup, eff, tasks, steals,
                    idle, imbal, depth, ok ? "yes" : "NO", extra.c_str());
        std::fflush(stdout);
        if (csv_)
            csv_ << workload << "," << policy << "," << threads << "," << ms << ","
                 << speedup << "," << eff << "," << tasks << "," << steals << "," << idle
                 << "," << imbal << "," << depth << "," << (ok ? 1 : 0) << "," << seq_ms << ",\"" << extra << "\"\n";
    }

private:
    std::ofstream csv_;
};

// Timing on a shared machine is noisy: another process can steal the CPU for
// part of a run, which only ever makes a run slower. So we take the fastest
// of several runs, and we time the sequential version again right next to
// every parallel configuration (paired runs), so both see the same machine state.
bool run_case(const Case& c, const BenchOptions& opts, Reporter& rep) {
    int reps = std::max(1, opts.reps);
    c.seq();   // warm-up: page in memory, fill caches
    double first_seq = 1e300;
    for (int r = 0; r < reps; r++) first_seq = std::min(first_seq, time_ms(c.seq));
    rep.row(c.name, "sequential", 1, first_seq, first_seq, nullptr, true, "");

    bool all_ok = true;
    const std::vector<Policy>& policies = c.only_policies.empty() ? opts.policies : c.only_policies;
    for (Policy p : policies) {
        for (int t : opts.threads) {
            Scheduler sched(t, p);
            bool ok = c.par(sched);   // warm-up, also starts the threads

            double seq_ms = 1e300;
            double best_ms = 1e300;
            RunStats best_stats;
            for (int r = 0; r < reps; r++) {
                seq_ms = std::min(seq_ms, time_ms(c.seq));
                TraceSnapshot before = sched.trace().snapshot();
                bool run_ok = true;
                double ms = time_ms([&] { run_ok = c.par(sched); });
                TraceSnapshot after = sched.trace().snapshot();
                ok = ok && run_ok;
                if (ms < best_ms) {
                    best_ms = ms;
                    best_stats = diff_stats(before, after, ms, t);
                }
            }
            rep.row(c.name, policy_name(p), t, best_ms, seq_ms, &best_stats, ok,
                    c.extra ? c.extra() : "");
            all_ok = all_ok && ok;
        }
    }

    if (!opts.trace_path.empty()) {
        int t = *std::max_element(opts.threads.begin(), opts.threads.end());
        Scheduler sched(t, Policy::WorkStealing, /*keep_events=*/true);
        c.par(sched);
        std::string path = opts.trace_path;
        size_t dot = path.rfind('.');
        std::string file = (dot == std::string::npos ? path : path.substr(0, dot)) + "_" +
                           c.name + ".csv";
        if (sched.trace().write_events_csv(file))
            std::printf("  trace written to %s\n", file.c_str());
    }
    return all_ok;
}

int pick(int value, int fallback) { return value > 0 ? value : fallback; }

// ---------- matrix ----------

std::vector<Case> matrix_cases(int n) {
    auto a = std::make_shared<Matrix>(random_matrix(n, 1));
    auto b = std::make_shared<Matrix>(random_matrix(n, 2));
    auto ref = std::make_shared<Matrix>(multiply_seq(*a, *b));
    Case c;
    c.name = "matrix-n" + std::to_string(n);
    c.seq = [=] { multiply_seq(*a, *b); };
    c.par = [=](Scheduler& s) {
        int grain = std::max(1, n / 64);
        return max_abs_diff(multiply_par(s, *a, *b, grain), *ref) == 0.0;
    };
    return {c};
}

bool bench_matrix(const BenchOptions& opts, Reporter& rep) {
    int n = pick(opts.size, 512);
    // Same arithmetic, different loop order: shows what cache misses cost.
    Matrix a = random_matrix(n, 1), b = random_matrix(n, 2);
    double naive = time_ms([&] { multiply_naive(a, b); });
    double good = time_ms([&] { multiply_seq(a, b); });
    std::printf("cache check n=%d: i-j-k %.1f ms, i-k-j %.1f ms (%.1fx)\n", n, naive, good,
                naive / good);
    bool ok = true;
    for (auto& c : matrix_cases(n)) ok = run_case(c, opts, rep) && ok;
    return ok;
}

// Small matrices: is parallel even worth it? This is what the adaptive
// threshold in multiply_adaptive is based on.
bool bench_matrix_sizes(const BenchOptions& opts, Reporter& rep) {
    BenchOptions o = opts;
    o.threads = {*std::max_element(opts.threads.begin(), opts.threads.end())};
    o.policies = {Policy::WorkStealing};
    o.reps = std::max(opts.reps, 9);
    bool ok = true;
    for (int n : {16, 32, 64, 128, 256}) {
        auto cases = matrix_cases(n);
        ok = run_case(cases[0], o, rep) && ok;
    }
    return ok;
}

// ---------- graph ----------

bool bench_graph(const BenchOptions& opts, Reporter& rep) {
    int n = pick(opts.size, 500000);
    bool ok = true;
    for (std::string kind : {"random", "skewed", "grid"}) {
        auto g = std::make_shared<Graph>(make_graph(kind, n, 7));
        auto ref = std::make_shared<std::vector<int>>(bfs_seq(*g, 0));
        Case c;
        c.name = "bfs-" + kind;
        c.seq = [=] { bfs_seq(*g, 0); };
        c.par = [=](Scheduler& s) { return bfs_par(s, *g, 0, 256).dist == *ref; };
        ok = run_case(c, opts, rep) && ok;

        if (kind == "random") {
            auto dups = std::make_shared<int64_t>(0);
            Case racy;
            racy.name = "bfs-random-racy";
            racy.seq = c.seq;
            racy.par = [=](Scheduler& s) {
                BfsResult r = bfs_racy(s, *g, 0, 64);
                *dups = r.duplicate_visits;
                return r.dist == *ref;
            };
            racy.extra = [=] { return "duplicate_visits=" + std::to_string(*dups); };
            racy.only_policies = {Policy::WorkStealing};
            ok = run_case(racy, opts, rep) && ok;
        }
    }
    return ok;
}

// ---------- chess ----------

bool bench_chess(const BenchOptions& opts, Reporter& rep) {
    int depth = pick(opts.size, 5);
    struct Named { const char* name; const char* fen; };
    const Named positions[] = {
        {"chess-start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1"},
        {"chess-middle", "r1bq1rk1/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 0 7"},
    };
    bool ok = true;
    for (const Named& p : positions) {
        auto pos = std::make_shared<Position>();
        parse_fen(p.fen, *pos);
        auto ref = std::make_shared<SearchResult>(search_seq(*pos, depth));
        auto last = std::make_shared<SearchResult>();
        Case c;
        c.name = std::string(p.name) + "-d" + std::to_string(depth);
        c.seq = [=] { search_seq(*pos, depth); };
        c.par = [=](Scheduler& s) {
            *last = search_par(s, *pos, depth);
            return last->score == ref->score;   // best move can differ on ties
        };
        c.extra = [=] {
            char buf[128];
            std::snprintf(buf, sizeof buf, "nodes=%lld search_overhead=%.2f best=%s",
                          (long long)last->nodes, (double)last->nodes / ref->nodes,
                          move_to_string(last->best).c_str());
            return std::string(buf);
        };
        ok = run_case(c, opts, rep) && ok;
    }
    return ok;
}

// ---------- swarm ----------

bool bench_swarm(const BenchOptions& opts, Reporter& rep) {
    SwarmConfig cfg;
    cfg.num_agents = pick(opts.size, 4000);
    const int steps = 60;
    SwarmState ref_state = make_swarm(cfg);
    run_swarm(ref_state, steps, nullptr, 64);
    uint64_t ref_hash = swarm_hash(ref_state);
    auto last = std::make_shared<StepStats>();

    Case c;
    c.name = "swarm-" + std::to_string(cfg.num_agents);
    c.seq = [=] {
        SwarmState s = make_swarm(cfg);
        run_swarm(s, steps, nullptr, 64);
    };
    c.par = [=](Scheduler& sched) {
        SwarmState s = make_swarm(cfg);
        *last = run_swarm(s, steps, &sched, 64);
        return swarm_hash(s) == ref_hash;   // bit-identical, not just close
    };
    c.extra = [=] {
        return "collisions=" + std::to_string(last->collisions) +
               " at_goal=" + std::to_string(last->at_goal);
    };
    return run_case(c, opts, rep);
}

// ---------- language ----------

std::string fib_program(int n) {
    return "let rec fib n = if n < 2 then n else "
           "let (a, b) = par(fib (n - 1), fib (n - 2)) in a + b in fib " +
           std::to_string(n);
}

Case lang_case(const std::string& name, int n, int cutoff) {
    auto program = std::make_shared<Program>(parse_program(fib_program(n)));
    Evaluator seq_eval(nullptr, 0);
    std::string expected = value_to_string(seq_eval.run(*program));
    auto spawned = std::make_shared<int64_t>(0);
    Case c;
    c.name = name;
    c.seq = [=] {
        Evaluator e(nullptr, 0);
        e.run(*program);
    };
    c.par = [=](Scheduler& s) {
        Evaluator e(&s, cutoff);
        bool ok = value_to_string(e.run(*program)) == expected;
        *spawned = e.tasks_spawned();
        return ok;
    };
    c.extra = [=] { return "par_tasks=" + std::to_string(*spawned); };
    return c;
}

bool bench_lang(const BenchOptions& opts, Reporter& rep) {
    int n = pick(opts.size, 24);
    return run_case(lang_case("lang-fib" + std::to_string(n), n, 10), opts, rep);
}

// How deep should par keep making tasks? Too shallow: not enough tasks.
// Too deep: millions of tiny tasks and the overhead wins.
bool bench_lang_cutoff(const BenchOptions& opts, Reporter& rep) {
    int n = pick(opts.size, 24);
    BenchOptions o = opts;
    o.threads = {*std::max_element(opts.threads.begin(), opts.threads.end())};
    o.policies = {Policy::SharedQueue, Policy::WorkStealing};
    bool ok = true;
    for (int cutoff : {1, 4, 8, 12, 16, 20}) {
        Case c = lang_case("lang-cut" + std::to_string(cutoff), n, cutoff);
        ok = run_case(c, o, rep) && ok;
    }
    return ok;
}

}  // namespace

int run_bench(const std::string& workload, const BenchOptions& opts) {
    Reporter rep(opts.csv_path);
    std::printf("hardware threads: %u, reps: %d (fastest run shown, speedup vs a paired sequential run)\n",
                std::thread::hardware_concurrency(), opts.reps);
    rep.header();

    bool ok = true;
    bool known = false;
    auto want = [&](const char* name) {
        bool yes = workload == name || workload == "all";
        known = known || yes;
        return yes;
    };
    if (want("matrix")) ok = bench_matrix(opts, rep) && ok;
    if (want("matrix-sizes")) ok = bench_matrix_sizes(opts, rep) && ok;
    if (want("graph")) ok = bench_graph(opts, rep) && ok;
    if (want("chess")) ok = bench_chess(opts, rep) && ok;
    if (want("swarm")) ok = bench_swarm(opts, rep) && ok;
    if (want("lang")) ok = bench_lang(opts, rep) && ok;
    if (want("lang-cutoff")) ok = bench_lang_cutoff(opts, rep) && ok;

    if (!known) {
        std::fprintf(stderr, "unknown workload '%s'\n", workload.c_str());
        return 2;
    }
    if (!ok) std::printf("\nWARNING: at least one parallel run gave a wrong answer\n");
    return ok ? 0 : 1;
}
