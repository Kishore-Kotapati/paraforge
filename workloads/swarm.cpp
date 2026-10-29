#include "swarm.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "../runtime/scheduler.h"

namespace {

// Uniform grid of cells, so each agent only checks nearby agents instead of
// all N (turns O(N^2) into roughly O(N)).
struct Grid {
    int cells_per_side = 1;
    double cell_size = 1;
    std::vector<int> cell_start;   // agents of cell c are order[cell_start[c] .. cell_start[c+1])
    std::vector<int> order;

    int cell_of(double x, double y) const {
        int cx = std::min(cells_per_side - 1, std::max(0, (int)(x / cell_size)));
        int cy = std::min(cells_per_side - 1, std::max(0, (int)(y / cell_size)));
        return cy * cells_per_side + cx;
    }
};

// Counting sort of agents into cells. Sequential: it is cheap compared to
// the update, and keeping it simple keeps the order deterministic.
void build_grid(const SwarmState& s, Grid& grid) {
    grid.cell_size = s.cfg.sense_radius;
    grid.cells_per_side = std::max(1, (int)(s.cfg.world / grid.cell_size));
    int cells = grid.cells_per_side * grid.cells_per_side;
    grid.cell_start.assign(cells + 1, 0);
    for (const Agent& a : s.agents) grid.cell_start[grid.cell_of(a.x, a.y) + 1]++;
    for (int c = 0; c < cells; c++) grid.cell_start[c + 1] += grid.cell_start[c];
    grid.order.resize(s.agents.size());
    std::vector<int> fill(grid.cell_start.begin(), grid.cell_start.end() - 1);
    for (int i = 0; i < (int)s.agents.size(); i++) {
        const Agent& a = s.agents[i];
        grid.order[fill[grid.cell_of(a.x, a.y)]++] = i;
    }
}

// New state of agent i. Reads only `cur`, writes nothing shared.
Agent update_agent(const SwarmState& s, const Grid& grid, int i, int64_t& close_pairs) {
    const SwarmConfig& cfg = s.cfg;
    const Agent& a = s.agents[i];

    // Pull toward the goal
    double gx = s.goal_x[a.goal] - a.x, gy = s.goal_y[a.goal] - a.y;
    double gdist = std::sqrt(gx * gx + gy * gy) + 1e-9;
    double ax = 0.05 * gx / gdist, ay = 0.05 * gy / gdist;

    // Push away from close neighbours (3x3 cells around this agent)
    int cx = std::min(grid.cells_per_side - 1, (int)(a.x / grid.cell_size));
    int cy = std::min(grid.cells_per_side - 1, (int)(a.y / grid.cell_size));
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= grid.cells_per_side || ny >= grid.cells_per_side)
                continue;
            int c = ny * grid.cells_per_side + nx;
            for (int k = grid.cell_start[c]; k < grid.cell_start[c + 1]; k++) {
                int j = grid.order[k];
                if (j == i) continue;
                double rx = a.x - s.agents[j].x, ry = a.y - s.agents[j].y;
                double d2 = rx * rx + ry * ry;
                if (d2 >= cfg.sense_radius * cfg.sense_radius || d2 == 0) continue;
                double d = std::sqrt(d2);
                double push = 0.15 * (cfg.sense_radius - d) / cfg.sense_radius;
                ax += push * rx / d;
                ay += push * ry / d;
                if (d < cfg.collide_radius && j > i) close_pairs++;   // count each pair once
            }
        }

    Agent next = a;
    next.vx = 0.9 * a.vx + ax;
    next.vy = 0.9 * a.vy + ay;
    double speed = std::sqrt(next.vx * next.vx + next.vy * next.vy);
    if (speed > cfg.max_speed) {
        next.vx *= cfg.max_speed / speed;
        next.vy *= cfg.max_speed / speed;
    }
    next.x = std::fmin(cfg.world - 1e-6, std::fmax(0.0, a.x + next.vx));
    next.y = std::fmin(cfg.world - 1e-6, std::fmax(0.0, a.y + next.vy));
    return next;
}

}  // namespace

SwarmState make_swarm(const SwarmConfig& cfg) {
    SwarmState s;
    s.cfg = cfg;
    uint32_t x = cfg.seed ? cfg.seed : 1;
    auto rnd = [&] {
        x = x * 1664525u + 1013904223u;
        return (double)(x >> 8) / (1u << 24);
    };
    // Four goals near the corners; agents start spread over the whole world.
    double w = cfg.world;
    s.goal_x = {0.15 * w, 0.85 * w, 0.15 * w, 0.85 * w};
    s.goal_y = {0.15 * w, 0.15 * w, 0.85 * w, 0.85 * w};
    for (int i = 0; i < cfg.num_agents; i++)
        s.agents.push_back({rnd() * w, rnd() * w, 0, 0, i % 4});
    return s;
}

StepStats run_swarm(SwarmState& state, int steps, Scheduler* sched, int grain) {
    std::vector<Agent> next(state.agents.size());
    Grid grid;
    StepStats stats;
    int n = (int)state.agents.size();

    for (int step = 0; step < steps; step++) {
        build_grid(state, grid);
        std::atomic<int64_t> collisions{0};

        auto update_range = [&](int64_t lo, int64_t hi) {
            int64_t local = 0;
            for (int64_t i = lo; i < hi; i++)
                next[i] = update_agent(state, grid, (int)i, local);
            collisions += local;   // one atomic add per chunk, not per agent
        };

        if (sched) sched->parallel_for(0, n, grain, update_range);
        else update_range(0, n);
        // parallel_for returning acts as the barrier between steps.

        state.agents.swap(next);
        stats.collisions = collisions.load();
    }

    stats.at_goal = 0;
    for (const Agent& a : state.agents) {
        double dx = a.x - state.goal_x[a.goal], dy = a.y - state.goal_y[a.goal];
        if (dx * dx + dy * dy < 9.0) stats.at_goal++;
    }
    return stats;
}

uint64_t swarm_hash(const SwarmState& state) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a over the raw bytes
    for (const Agent& a : state.agents) {
        double vals[2] = {a.x, a.y};
        unsigned char bytes[sizeof(vals)];
        std::memcpy(bytes, vals, sizeof(vals));
        for (unsigned char b : bytes) {
            h ^= b;
            h *= 1099511628211ull;
        }
    }
    return h;
}
