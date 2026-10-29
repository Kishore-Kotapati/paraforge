// Swarm of robots moving to goals while keeping apart: the "lock-step" workload.
//
// Every step reads the old positions and writes new ones into a second
// buffer (double buffering). No agent ever reads a half-updated neighbour,
// so the result is bit-identical for any thread count or policy.
#pragma once

#include <cstdint>
#include <vector>

class Scheduler;

struct Agent {
    double x, y, vx, vy;
    int goal;   // index into SwarmConfig goals
};

struct SwarmConfig {
    int num_agents = 2000;
    double world = 200.0;           // square world [0, world)
    double sense_radius = 4.0;      // neighbours closer than this push back
    double collide_radius = 1.0;    // closer than this counts as a collision
    double max_speed = 1.5;
    uint32_t seed = 42;
};

struct SwarmState {
    SwarmConfig cfg;
    std::vector<Agent> agents;
    std::vector<double> goal_x, goal_y;
};

struct StepStats {
    int64_t collisions = 0;   // close pairs this step
    int at_goal = 0;          // agents within 3 units of their goal
};

SwarmState make_swarm(const SwarmConfig& cfg);

// Run `steps` steps. sched == nullptr runs sequentially.
StepStats run_swarm(SwarmState& state, int steps, Scheduler* sched, int grain);

// Hash of all positions, used to check that runs are identical.
uint64_t swarm_hash(const SwarmState& state);
