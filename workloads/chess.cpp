#include "chess.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>

#include "../runtime/scheduler.h"

namespace {

const int kKnightSteps[8] = {33, 31, 18, 14, -33, -31, -18, -14};
const int kBishopSteps[4] = {17, 15, -17, -15};
const int kRookSteps[4] = {16, -16, 1, -1};
const int kKingSteps[8] = {17, 15, -17, -15, 16, -16, 1, -1};
const int kPieceValue[7] = {0, 100, 320, 330, 500, 900, 0};

inline bool on_board(int sq) { return (sq & 0x88) == 0; }
inline int type_of(int8_t p) { return p < 0 ? -p : p; }
inline int color_of(int8_t p) { return p > 0 ? 1 : (p < 0 ? -1 : 0); }

void add_pawn_moves(const Position& pos, int from, std::vector<Move>& out) {
    int side = pos.side;
    int forward = side == 1 ? 16 : -16;
    int start_rank = side == 1 ? 1 : 6;
    int last_rank = side == 1 ? 7 : 0;

    auto push = [&](int to, int captured) {
        if ((to >> 4) == last_rank) {
            for (int promo : {QUEEN, ROOK, BISHOP, KNIGHT})
                out.push_back({(uint8_t)from, (uint8_t)to, (int8_t)promo, (int8_t)captured});
        } else {
            out.push_back({(uint8_t)from, (uint8_t)to, 0, (int8_t)captured});
        }
    };

    int one = from + forward;
    if (on_board(one) && pos.board[one] == EMPTY) {
        push(one, 0);
        int two = one + forward;
        if ((from >> 4) == start_rank && pos.board[two] == EMPTY) push(two, 0);
    }
    for (int diag : {forward - 1, forward + 1}) {
        int to = from + diag;
        if (on_board(to) && color_of(pos.board[to]) == -side)
            push(to, type_of(pos.board[to]));
    }
}

void add_step_moves(const Position& pos, int from, const int* steps, int count,
                    bool slide, std::vector<Move>& out) {
    for (int i = 0; i < count; i++) {
        int to = from + steps[i];
        while (on_board(to)) {
            int8_t target = pos.board[to];
            if (color_of(target) == pos.side) break;
            out.push_back({(uint8_t)from, (uint8_t)to, 0, (int8_t)type_of(target)});
            if (target != EMPTY || !slide) break;
            to += steps[i];
        }
    }
}

// Is `sq` attacked by any piece of colour `by`?
bool is_attacked(const Position& pos, int sq, int by) {
    // Pawns: look backwards from the square to where an attacking pawn would be.
    int pawn_dir = by == 1 ? -16 : 16;
    for (int d : {pawn_dir - 1, pawn_dir + 1}) {
        int s = sq + d;
        if (on_board(s) && pos.board[s] == by * PAWN) return true;
    }
    for (int d : kKnightSteps) {
        int s = sq + d;
        if (on_board(s) && pos.board[s] == by * KNIGHT) return true;
    }
    for (int d : kKingSteps) {
        int s = sq + d;
        if (on_board(s) && pos.board[s] == by * KING) return true;
    }
    for (int d : kBishopSteps) {
        for (int s = sq + d; on_board(s); s += d) {
            int8_t p = pos.board[s];
            if (p == by * BISHOP || p == by * QUEEN) return true;
            if (p != EMPTY) break;
        }
    }
    for (int d : kRookSteps) {
        for (int s = sq + d; on_board(s); s += d) {
            int8_t p = pos.board[s];
            if (p == by * ROOK || p == by * QUEEN) return true;
            if (p != EMPTY) break;
        }
    }
    return false;
}

// Captures first, most valuable victim first. Good ordering is what makes
// alpha-beta prune, and it also decides which move YBW searches alone.
void order_moves(std::vector<Move>& moves) {
    std::stable_sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
        int sa = kPieceValue[a.captured] * 10 + kPieceValue[a.promo];
        int sb = kPieceValue[b.captured] * 10 + kPieceValue[b.promo];
        return sa > sb;
    });
}

int alpha_beta(const Position& pos, int depth, int alpha, int beta, int ply,
               int64_t& nodes) {
    nodes++;
    if (depth == 0) return evaluate(pos);

    std::vector<Move> moves;
    generate_moves(pos, moves);
    order_moves(moves);

    bool any_legal = false;
    for (const Move& m : moves) {
        Position child = make_move(pos, m);
        if (in_check(child, pos.side)) continue;   // illegal: left own king in check
        any_legal = true;
        int score = -alpha_beta(child, depth - 1, -beta, -alpha, ply + 1, nodes);
        if (score >= beta) return score;   // opponent will avoid this line
        if (score > alpha) alpha = score;
    }
    if (!any_legal) {
        // Mate is worse the sooner it happens, so prefer the fastest mate.
        return in_check(pos, pos.side) ? -kMateScore + ply : 0;
    }
    return alpha;
}

std::vector<Move> ordered_legal_moves(const Position& pos) {
    std::vector<Move> moves = legal_moves(pos);
    order_moves(moves);
    return moves;
}

}  // namespace

Position start_position() {
    Position pos;
    parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1", pos);
    return pos;
}

bool parse_fen(const std::string& fen, Position& out) {
    Position pos;
    std::istringstream in(fen);
    std::string placement, side;
    if (!(in >> placement >> side)) return false;

    int rank = 7, file = 0;
    for (char ch : placement) {
        if (ch == '/') {
            rank--;
            file = 0;
        } else if (ch >= '1' && ch <= '8') {
            file += ch - '0';
        } else {
            const char* letters = "pnbrqk";
            const char* found = nullptr;
            for (const char* p = letters; *p; p++)
                if (*p == (ch | 32)) found = p;   // | 32 turns 'P' into 'p'
            if (!found || rank < 0 || file > 7) return false;
            int type = (int)(found - letters) + 1;
            pos.board[rank * 16 + file] = (int8_t)((ch >= 'a') ? -type : type);
            file++;
        }
    }
    if (side != "w" && side != "b") return false;
    pos.side = side == "w" ? 1 : -1;
    out = pos;
    return true;
}

void generate_moves(const Position& pos, std::vector<Move>& out) {
    out.clear();
    for (int sq = 0; sq < 128; sq++) {
        if (!on_board(sq)) {
            sq += 7;   // skip the 8 off-board squares of this rank
            continue;
        }
        int8_t p = pos.board[sq];
        if (color_of(p) != pos.side) continue;
        switch (type_of(p)) {
            case PAWN: add_pawn_moves(pos, sq, out); break;
            case KNIGHT: add_step_moves(pos, sq, kKnightSteps, 8, false, out); break;
            case BISHOP: add_step_moves(pos, sq, kBishopSteps, 4, true, out); break;
            case ROOK: add_step_moves(pos, sq, kRookSteps, 4, true, out); break;
            case QUEEN: add_step_moves(pos, sq, kKingSteps, 8, true, out); break;
            case KING: add_step_moves(pos, sq, kKingSteps, 8, false, out); break;
        }
    }
}

Position make_move(const Position& pos, const Move& m) {
    Position next = pos;
    int8_t piece = next.board[m.from];
    next.board[m.from] = EMPTY;
    next.board[m.to] = m.promo ? (int8_t)(pos.side * m.promo) : piece;
    next.side = -pos.side;
    return next;
}

bool in_check(const Position& pos, int side) {
    for (int sq = 0; sq < 128; sq++)
        if (on_board(sq) && pos.board[sq] == side * KING)
            return is_attacked(pos, sq, -side);
    return false;   // no king (only in test positions)
}

std::vector<Move> legal_moves(const Position& pos) {
    std::vector<Move> moves, legal;
    generate_moves(pos, moves);
    for (const Move& m : moves)
        if (!in_check(make_move(pos, m), pos.side)) legal.push_back(m);
    return legal;
}

std::string move_to_string(const Move& m) {
    std::string s;
    s += (char)('a' + (m.from & 7));
    s += (char)('1' + (m.from >> 4));
    s += (char)('a' + (m.to & 7));
    s += (char)('1' + (m.to >> 4));
    if (m.promo) s += " pnbrqk"[m.promo];
    return s;
}

bool parse_move(const Position& pos, const std::string& s, Move& out) {
    for (const Move& m : legal_moves(pos)) {
        std::string ms = move_to_string(m);
        // "e7e8" without a letter means queen promotion
        if (ms == s || (m.promo == QUEEN && ms == s + "q")) {
            out = m;
            return true;
        }
    }
    return false;
}

uint64_t perft(const Position& pos, int depth) {
    if (depth == 0) return 1;
    std::vector<Move> moves;
    generate_moves(pos, moves);
    uint64_t count = 0;
    for (const Move& m : moves) {
        Position child = make_move(pos, m);
        if (!in_check(child, pos.side)) count += perft(child, depth - 1);
    }
    return count;
}

int evaluate(const Position& pos) {
    int score = 0;
    for (int sq = 0; sq < 128; sq++) {
        if (!on_board(sq) || pos.board[sq] == EMPTY) continue;
        int8_t p = pos.board[sq];
        int type = type_of(p), color = color_of(p);
        int value = kPieceValue[type];
        int rank = sq >> 4, file = sq & 7;
        // Small bonuses: advanced pawns, knights and bishops near the centre.
        if (type == PAWN) value += 5 * (color == 1 ? rank - 1 : 6 - rank);
        if (type == KNIGHT || type == BISHOP) {
            int centre_dist = std::abs(2 * file - 7) + std::abs(2 * rank - 7);
            value += 14 - 2 * centre_dist;
        }
        score += color * value;
    }
    return score * pos.side;
}

SearchResult search_seq(const Position& pos, int depth) {
    SearchResult result;
    std::vector<Move> moves = ordered_legal_moves(pos);
    if (moves.empty()) {
        result.score = in_check(pos, pos.side) ? -kMateScore : 0;
        return result;
    }
    int alpha = -kMateScore - 1;
    const int beta = kMateScore + 1;
    for (const Move& m : moves) {
        int score = -alpha_beta(make_move(pos, m), depth - 1, -beta, -alpha, 1, result.nodes);
        result.root.push_back({m, score, score > alpha, -1});
        if (score > alpha) {
            alpha = score;
            result.best = m;
            result.has_move = true;
        }
    }
    result.score = alpha;
    return result;
}

SearchResult search_par(Scheduler& sched, const Position& pos, int depth) {
    SearchResult result;
    std::vector<Move> moves = ordered_legal_moves(pos);
    if (moves.empty()) {
        result.score = in_check(pos, pos.side) ? -kMateScore : 0;
        return result;
    }
    const int beta = kMateScore + 1;

    // Step 1: first move alone, full window. Its score becomes the bound
    // every other root move has to beat.
    int64_t first_nodes = 0;
    int first_score = -alpha_beta(make_move(pos, moves[0]), depth - 1, -beta,
                                  kMateScore + 1, 1, first_nodes);

    std::atomic<int> best_score{first_score};
    std::atomic<int64_t> total_nodes{first_nodes};
    std::mutex best_mutex;
    Move best_move = moves[0];
    int locked_score = first_score;   // guarded by best_mutex, like best_move
    int best_index = 0;
    result.root.resize(moves.size());
    result.root[0] = {moves[0], first_score, true, sched.current_worker()};

    // Step 2: the rest in parallel. Each task owns its board copy, so the
    // only shared data are the atomics and the mutex-protected best move.
    // Spawned last-to-first: a worker pops its own queue newest-first, so it
    // still searches in the good order, while thieves take the late moves.
    TaskGroup group;
    for (size_t i = moves.size() - 1; i >= 1; i--) {
        sched.spawn(group, [&, i] {
            int64_t nodes = 0;
            int alpha = best_score.load(std::memory_order_relaxed);
            int score = -alpha_beta(make_move(pos, moves[i]), depth - 1, -beta, -alpha, 1, nodes);
            total_nodes += nodes;
            result.root[i] = {moves[i], score, score > alpha, sched.current_worker()};

            // Raise best_score only if we beat it (lock-free "atomic max").
            int current = best_score.load();
            while (score > current && !best_score.compare_exchange_weak(current, score)) {
            }
            if (score > alpha) {
                std::lock_guard<std::mutex> lock(best_mutex);
                // On equal scores prefer the earlier (better ordered) move.
                if (score > locked_score || (score == locked_score && (int)i < best_index)) {
                    locked_score = score;
                    best_move = moves[i];
                    best_index = (int)i;
                }
            }
        });
    }
    sched.wait(group);

    // A task can return a score equal to the bound it started with without
    // having beaten it, so trust best_score for the value.
    result.score = best_score.load();
    if (result.score == first_score) best_move = moves[0];
    result.best = best_move;
    result.has_move = true;
    result.nodes = total_nodes.load();
    return result;
}
