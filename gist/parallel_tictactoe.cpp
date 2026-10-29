// tic tac toe minimax on one thread and on many threads
// compile with pthread and pass the thread count when you run it

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

typedef std::array<char, 9> Grid;

static const int winLines[8][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8},
                                   {0, 3, 6}, {1, 4, 7}, {2, 5, 8},
                                   {0, 4, 8}, {2, 4, 6}};

char whoWon(const Grid& g) {
    for (int k = 0; k < 8; k++) {
        int p = winLines[k][0], q = winLines[k][1], r = winLines[k][2];
        if (g[p] != ' ' && g[p] == g[q] && g[q] == g[r])
            return g[p];
    }
    return ' ';
}

// score is 1 if X wins and minus 1 if O wins and 0 for a draw
int minimax(Grid& g, char turn, long& cnt) {
    cnt++;
    char w = whoWon(g);
    if (w == 'X') return 1;
    if (w == 'O') return -1;

    char nxt = (turn == 'X') ? 'O' : 'X';
    int bst = (turn == 'X') ? -2 : 2;
    bool anyMove = false;

    for (int c = 0; c < 9; c++) {
        if (g[c] != ' ') continue;
        anyMove = true;
        g[c] = turn;
        int val = minimax(g, nxt, cnt);
        g[c] = ' ';
        if (turn == 'X') bst = std::max(bst, val);
        else             bst = std::min(bst, val);
    }
    if (!anyMove) return 0;
    return bst;
}

struct Job { int xm; int om; };   // X first move and O reply
struct Out { int score = 0; long nodes = 0; };

std::vector<Job> buildJobs() {
    std::vector<Job> v;
    for (int a = 0; a < 9; a++)
        for (int b = 0; b < 9; b++)
            if (a != b) v.push_back({a, b});
    return v;
}

Out runJob(const Job& j) {
    Grid g;
    g.fill(' ');
    g[j.xm] = 'X';
    g[j.om] = 'O';
    Out o;
    o.score = minimax(g, 'X', o.nodes);
    return o;
}

std::vector<Out> solveSeq(const std::vector<Job>& jobs) {
    std::vector<Out> res(jobs.size());
    for (size_t i = 0; i < jobs.size(); i++) res[i] = runJob(jobs[i]);
    return res;
}

std::vector<Out> solvePar(const std::vector<Job>& jobs, int nThr) {
    std::vector<Out> res(jobs.size());
    std::atomic<size_t> ticket{0};   // next job to hand out

    auto work = [&]() {
        while (true) {
            size_t my = ticket.fetch_add(1);
            if (my >= jobs.size()) break;
            res[my] = runJob(jobs[my]);
        }
    };

    std::vector<std::thread> pool;
    for (int t = 0; t < nThr; t++) pool.emplace_back(work);
    for (auto& th : pool) th.join();
    return res;
}

std::array<int, 9> firstMoveValues(const std::vector<Job>& jobs, const std::vector<Out>& res) {
    std::array<int, 9> val;
    val.fill(2);
    for (size_t i = 0; i < jobs.size(); i++) {
        int sq = jobs[i].xm;
        if (res[i].score < val[sq]) val[sq] = res[i].score;
    }
    return val;
}

template <typename Fn>
double timeIt(Fn f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main(int argc, char** argv) {
    int nThr = (int)std::thread::hardware_concurrency();
    if (argc > 1) nThr = std::atoi(argv[1]);
    if (nThr < 1) nThr = 1;

    const int reps = 20;
    std::vector<Job> jobs = buildJobs();
    std::vector<Out> s, p;   // sequential and parallel results

    double tSeq = timeIt([&] { for (int r = 0; r < reps; r++) s = solveSeq(jobs); });
    double tPar = timeIt([&] { for (int r = 0; r < reps; r++) p = solvePar(jobs, nThr); });

    long ns = 0, np = 0;
    bool ok = true;
    for (size_t i = 0; i < jobs.size(); i++) {
        ns += s[i].nodes;
        np += p[i].nodes;
        if (s[i].score != p[i].score || s[i].nodes != p[i].nodes) ok = false;
    }

    std::array<int, 9> v = firstMoveValues(jobs, s);
    const char* lbl[] = {"O wins", "draw", "X wins"};

    printf("first move results for X\n");
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++)
            printf("  %-7s", lbl[v[row * 3 + col] + 1]);
        printf("\n");
    }
    int top = *std::max_element(v.begin(), v.end());
    printf("\nperfect play ends in a %s\n\n", lbl[top + 1]);

    printf("jobs %zu\n", jobs.size());
    printf("positions checked %ld\n", ns);
    printf("one thread took %.1f ms\n", tSeq);
    printf("%d threads took %.1f ms\n", nThr, tPar);
    printf("speedup %.2f times\n", tSeq / tPar);
    printf("both match %s\n", (ok && ns == np) ? "yes" : "no");
    return ok ? 0 : 1;
}