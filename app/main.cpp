// ParaForge command line.
//
//   paraforge bench <workload> [options]   run benchmarks (see bench/bench.h)
//   paraforge run <file.pf> [options]      run a PFL program on the runtime
//   paraforge engine [options]             text chess engine (used by the AR viewer)
//   paraforge perft <depth>                count chess positions (move generator check)
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../bench/bench.h"
#include "../lang/lang.h"
#include "../workloads/chess.h"

namespace {

void usage() {
    std::printf(
        "usage:\n"
        "  paraforge bench <matrix|matrix-sizes|graph|chess|swarm|lang|lang-cutoff|all>\n"
        "        [--threads 1,2,4,8] [--policy static,shared,stealing] [--size N]\n"
        "        [--reps 5] [--csv results.csv] [--trace trace.csv]\n"
        "  paraforge run <file.pf> [--threads 4] [--policy stealing] [--cutoff 10]\n"
        "  paraforge engine [--threads 4] [--depth 4]\n"
        "  paraforge perft <depth>\n");
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        if (!item.empty()) parts.push_back(item);
    return parts;
}

// Reads "--name value" pairs. Returns false on an unknown or incomplete flag.
struct Flags {
    std::vector<std::pair<std::string, std::string>> items;
    bool parse(int argc, char** argv, int start) {
        for (int i = start; i < argc; i++) {
            std::string key = argv[i];
            if (key.rfind("--", 0) != 0 || i + 1 >= argc) {
                std::fprintf(stderr, "bad argument: %s\n", key.c_str());
                return false;
            }
            items.push_back({key.substr(2), argv[++i]});
        }
        return true;
    }
    std::string get(const std::string& key, const std::string& fallback) const {
        for (auto& kv : items)
            if (kv.first == key) return kv.second;
        return fallback;
    }
};

int cmd_bench(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    Flags flags;
    if (!flags.parse(argc, argv, 3)) return 2;

    BenchOptions opts;
    try {
        std::string threads = flags.get("threads", "");
        if (!threads.empty()) {
            opts.threads.clear();
            for (auto& t : split(threads, ',')) opts.threads.push_back(std::stoi(t));
        }
        std::string policies = flags.get("policy", "");
        if (!policies.empty()) {
            opts.policies.clear();
            for (auto& p : split(policies, ',')) {
                Policy pol;
                if (!parse_policy(p, pol)) {
                    std::fprintf(stderr, "unknown policy: %s\n", p.c_str());
                    return 2;
                }
                opts.policies.push_back(pol);
            }
        }
        opts.size = std::stoi(flags.get("size", "-1"));
        opts.reps = std::stoi(flags.get("reps", "5"));
    } catch (const std::exception&) {
        std::fprintf(stderr, "numbers expected for --threads/--size/--reps\n");
        return 2;
    }
    if (opts.threads.empty() || opts.policies.empty()) { usage(); return 2; }
    opts.csv_path = flags.get("csv", "");
    opts.trace_path = flags.get("trace", "");
    return run_bench(argv[2], opts);
}

int cmd_run(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    Flags flags;
    if (!flags.parse(argc, argv, 3)) return 2;

    std::ifstream in(argv[2]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    std::stringstream buf;
    buf << in.rdbuf();

    Policy policy;
    if (!parse_policy(flags.get("policy", "stealing"), policy)) {
        std::fprintf(stderr, "unknown policy\n");
        return 2;
    }
    int threads = std::stoi(flags.get("threads", "4"));
    int cutoff = std::stoi(flags.get("cutoff", "10"));

    try {
        Program program = parse_program(buf.str());
        Scheduler sched(threads, policy);
        Evaluator eval(&sched, cutoff);
        auto t0 = std::chrono::steady_clock::now();
        ValuePtr result = eval.run(program);
        auto t1 = std::chrono::steady_clock::now();
        std::printf("%s\n", value_to_string(result).c_str());
        std::fprintf(stderr, "[%.2f ms, %d threads, %s, %lld par tasks]\n",
                     std::chrono::duration<double, std::milli>(t1 - t0).count(), threads,
                     policy_name(policy), (long long)eval.tasks_spawned());
    } catch (const LangError& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

void print_board(const Position& pos) {
    for (int rank = 7; rank >= 0; rank--) {
        std::printf("%d ", rank + 1);
        for (int file = 0; file < 8; file++) {
            int8_t p = pos.board[rank * 16 + file];
            char c = ".PNBRQK"[p < 0 ? -p : p];
            if (p < 0) c = (char)(c | 32);   // lowercase for black
            std::printf("%c ", c);
        }
        std::printf("\n");
    }
    std::printf("  a b c d e f g h   (%s to move)\n", pos.side == 1 ? "white" : "black");
}

// Simple line protocol, close to UCI so the AR viewer stays small.
int cmd_engine(int argc, char** argv) {
    Flags flags;
    if (!flags.parse(argc, argv, 2)) return 2;
    int threads = std::stoi(flags.get("threads", "4"));
    int default_depth = std::stoi(flags.get("depth", "4"));

    Scheduler sched(threads, Policy::WorkStealing);
    Position pos = start_position();
    std::printf("ready threads=%d\n", threads);
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "quit") break;

        if (cmd == "position") {
            // position startpos [moves ...] | position fen <6 fields> [moves ...]
            std::string kind, word;
            in >> kind;
            bool ok = true;
            if (kind == "startpos") {
                pos = start_position();
            } else if (kind == "fen") {
                std::string fen;
                for (int i = 0; i < 6 && in >> word; i++) fen += word + " ";
                ok = parse_fen(fen, pos);
            } else {
                ok = false;
            }
            if (in >> word && word == "moves") {
                while (ok && in >> word) {
                    Move m;
                    ok = parse_move(pos, word, m);
                    if (ok) pos = make_move(pos, m);
                }
            }
            std::printf(ok ? "ok\n" : "error bad position\n");
        } else if (cmd == "move") {
            std::string s;
            in >> s;
            Move m;
            if (parse_move(pos, s, m)) {
                pos = make_move(pos, m);
                std::printf("ok\n");
            } else {
                std::printf("illegal\n");
            }
        } else if (cmd == "go") {
            std::string word;
            int depth = default_depth;
            if (in >> word && word == "depth") in >> depth;
            auto t0 = std::chrono::steady_clock::now();
            SearchResult r = search_par(sched, pos, depth < 1 ? 1 : depth);
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
            // Which worker searched which root move: the viewer colours by this.
            for (const RootMoveInfo& info : r.root)
                std::printf("info root %s score %d %s worker %d\n",
                            move_to_string(info.move).c_str(), info.score,
                            info.exact ? "exact" : "upper", info.worker);
            std::printf("info score %d nodes %lld time_ms %.1f\n", r.score,
                        (long long)r.nodes, ms);
            std::printf("bestmove %s\n", r.has_move ? move_to_string(r.best).c_str() : "none");
        } else if (cmd == "legal") {
            for (const Move& m : legal_moves(pos)) std::printf("%s ", move_to_string(m).c_str());
            std::printf("\n");
        } else if (cmd == "board") {
            print_board(pos);
        } else if (!cmd.empty()) {
            std::printf("error unknown command\n");
        }
        std::fflush(stdout);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string cmd = argv[1];
    try {
        if (cmd == "bench") return cmd_bench(argc, argv);
        if (cmd == "run") return cmd_run(argc, argv);
        if (cmd == "engine") return cmd_engine(argc, argv);
        if (cmd == "perft" && argc >= 3) {
            int depth = std::stoi(argv[2]);
            auto t0 = std::chrono::steady_clock::now();
            unsigned long long n = perft(start_position(), depth);
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
            std::printf("perft(%d) = %llu  (%.1f ms)\n", depth, n, ms);
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
