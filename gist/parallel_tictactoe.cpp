// Solves tic-tac-toe completely with minimax, first on one thread, then on
// several, and checks that both give the same answer.
//
// The game tree is split into tasks, one per pair of opening moves
// (X's first move, O's reply): 72 tasks. Worker threads take the next task
// from a shared atomic counter, so a thread that finishes early simply grabs
// more work. Each task writes only its own result slot, so no locks are needed.
//
//   g++ -std=c++17 -O2 -pthread parallel_tictactoe.cpp -o tictactoe
//   ./tictactoe 4          (number of threads, default = hardware threads)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Board = std::array<char, 9>;   // 'X', 'O' or ' '

char winner(const Board& b) {
    static const int lines[8][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {0, 3, 6},
                                    {1, 4, 7}, {2, 5, 8}, {0, 4, 8}, {2, 4, 6}};
    for (auto& l : lines)
        if (b[l[0]] != ' ' && b[l[0]] == b[l[1]] && b[l[1]] == b[l[2]]) return b[l[0]];
    return ' ';
}

// Score from X's point of view: +1 X wins, -1 O wins, 0 draw.
// Plain minimax without pruning, so every task does a fixed amount of work
// and the node counts of both versions must match exactly.
int minimax(Board& b, char player, long& nodes) {
    nodes++;
    char w = winner(b);
    if (w == 'X') return 1;
    if (w == 'O') return -1;

    int best = player == 'X' ? -2 : 2;
    bool moved = false;
    for (int sq = 0; sq < 9; sq++) {
        if (b[sq] != ' ') continue;
        moved = true;
        b[sq] = player;                         // try the move...
        int score = minimax(b, player == 'X' ? 'O' : 'X', nodes);
        b[sq] = ' ';                            // ...and undo it
        best = player == 'X' ? std::max(best, score) : std::min(best, score);
    }
    return moved ? best : 0;                    // full board, no winner: draw
}

struct Task {
    int x_move, o_reply;
};

struct Result {
    int score = 0;
    long nodes = 0;
};

std::vector<Task> make_tasks() {
    std::vector<Task> tasks;
    for (int x = 0; x < 9; x++)
        for (int o = 0; o < 9; o++)
            if (o != x) tasks.push_back({x, o});
    return tasks;
}

Result solve_task(const Task& t) {
    Board b;
    b.fill(' ');
    b[t.x_move] = 'X';
    b[t.o_reply] = 'O';
    Result r;
    r.score = minimax(b, 'X', r.nodes);
    return r;
}

std::vector<Result> solve_sequential(const std::vector<Task>& tasks) {
    std::vector<Result> results(tasks.size());
    for (size_t i = 0; i < tasks.size(); i++) results[i] = solve_task(tasks[i]);
    return results;
}

std::vector<Result> solve_parallel(const std::vector<Task>& tasks, int num_threads) {
    std::vector<Result> results(tasks.size());
    std::atomic<size_t> next{0};

    auto worker = [&] {
        // fetch_add hands out each index exactly once, even when two threads
        // ask at the same moment.
        for (size_t i = next.fetch_add(1); i < tasks.size(); i = next.fetch_add(1))
            results[i] = solve_task(tasks[i]);   // slot i belongs to this thread only
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) threads.emplace_back(worker);
    for (auto& th : threads) th.join();          // join = all results are written
    return results;
}

// Root score of each X opening: O picks the reply that is worst for X.
std::array<int, 9> opening_scores(const std::vector<Task>& tasks,
                                  const std::vector<Result>& results) {
    std::array<int, 9> score;
    score.fill(2);
    for (size_t i = 0; i < tasks.size(); i++)
        score[tasks[i].x_move] = std::min(score[tasks[i].x_move], results[i].score);
    return score;
}

template <typename F>
double time_ms(F&& fn) {
    auto t0 = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
}

int main(int argc, char** argv) {
    int threads = argc > 1 ? std::atoi(argv[1]) : (int)std::thread::hardware_concurrency();
    if (threads < 1) threads = 1;
    const int rounds = 20;   // repeat so the timing is not just thread start-up
    std::vector<Task> tasks = make_tasks();

    std::vector<Result> seq, par;
    double seq_ms = time_ms([&] { for (int r = 0; r < rounds; r++) seq = solve_sequential(tasks); });
    double par_ms = time_ms([&] { for (int r = 0; r < rounds; r++) par = solve_parallel(tasks, threads); });

    long seq_nodes = 0, par_nodes = 0;
    bool same = true;
    for (size_t i = 0; i < tasks.size(); i++) {
        seq_nodes += seq[i].nodes;
        par_nodes += par[i].nodes;
        same = same && seq[i].score == par[i].score && seq[i].nodes == par[i].nodes;
    }

    std::array<int, 9> scores = opening_scores(tasks, seq);
    const char* names[] = {"O wins", "draw", "X wins"};
    std::printf("Value of each first move for X (perfect play after it):\n");
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) std::printf("  %-7s", names[scores[row * 3 + col] + 1]);
        std::printf("\n");
    }
    int best = *std::max_element(scores.begin(), scores.end());
    std::printf("Tic-tac-toe is a %s with perfect play.\n\n", names[best + 1]);

    std::printf("tasks: %zu, positions searched per round: %ld\n", tasks.size(), seq_nodes);
    std::printf("sequential: %8.1f ms\n", seq_ms);
    std::printf("parallel  : %8.1f ms  (%d threads, speedup %.2fx)\n", par_ms, threads,
                seq_ms / par_ms);
    std::printf("results identical: %s\n", same && seq_nodes == par_nodes ? "yes" : "NO");
    return same ? 0 : 1;
}
