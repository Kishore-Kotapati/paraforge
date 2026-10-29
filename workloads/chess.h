// Chess move generation and alpha-beta search: the "uneven tree" workload.
//
// Board: 0x88 layout (square = rank * 16 + file). A square is on the board
// exactly when (sq & 0x88) == 0, which makes edge checks one AND.
//
// Not implemented (to keep the code short): castling and en passant.
// Perft from the start position is still exact up to depth 4, because
// neither move is possible that early.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Scheduler;

enum Piece : int8_t { EMPTY = 0, PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
// White pieces are positive, black pieces negative.

struct Move {
    uint8_t from = 0, to = 0;
    int8_t promo = 0;      // piece type promoted to, 0 if none
    int8_t captured = 0;   // piece type captured, 0 if none
    bool operator==(const Move& o) const {
        return from == o.from && to == o.to && promo == o.promo;
    }
};

struct Position {
    int8_t board[128] = {0};
    int side = 1;   // 1 = white to move, -1 = black
};

Position start_position();
bool parse_fen(const std::string& fen, Position& out);   // castling/ep fields ignored

void generate_moves(const Position& pos, std::vector<Move>& out);  // pseudo-legal
std::vector<Move> legal_moves(const Position& pos);
Position make_move(const Position& pos, const Move& m);   // copy-make: input untouched
bool in_check(const Position& pos, int side);

std::string move_to_string(const Move& m);                 // "e2e4", "e7e8q"
bool parse_move(const Position& pos, const std::string& s, Move& out);

uint64_t perft(const Position& pos, int depth);
int evaluate(const Position& pos);   // centipawns, from side-to-move's view

constexpr int kMateScore = 100000;

struct RootMoveInfo {
    Move move;
    int score = 0;
    bool exact = false;  // false: the move did not beat the bound it was searched
                         // with, so score is only an upper limit
    int worker = 0;      // which worker searched it (-1 = calling thread)
};

struct SearchResult {
    Move best;
    int score = 0;
    int64_t nodes = 0;
    bool has_move = false;
    std::vector<RootMoveInfo> root;
};

SearchResult search_seq(const Position& pos, int depth);

// Root splitting with "young brothers wait": search the first (best-ordered)
// root move alone to get a good bound, then search the rest in parallel,
// each starting from the best score found so far.
SearchResult search_par(Scheduler& sched, const Position& pos, int depth);
