// Breadth-first search (shortest hop distance): the "messy work" workload.
// Vertices have very different numbers of neighbours, and all threads share
// one distance array, so this is where races and imbalance show up.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Scheduler;

// Compressed sparse row: neighbours of v are adj[offset[v] .. offset[v+1]).
struct Graph {
    int num_vertices = 0;
    std::vector<int64_t> offset;
    std::vector<int> adj;
    int64_t num_edges() const { return (int64_t)adj.size(); }
};

Graph random_graph(int n, int avg_degree, uint32_t seed);   // even degrees
Graph skewed_graph(int n, int avg_degree, uint32_t seed);   // a few huge hubs
Graph grid_graph(int width, int height);                    // long, thin BFS
Graph make_graph(const std::string& kind, int n, uint32_t seed);

std::vector<int> bfs_seq(const Graph& g, int source);

struct BfsResult {
    std::vector<int> dist;
    int64_t duplicate_visits = 0;   // vertices added to a frontier twice
};

// Correct version: compare-and-swap decides which thread claims a vertex.
BfsResult bfs_par(Scheduler& sched, const Graph& g, int source, int grain);

// Deliberately wrong "check, then set" version, kept to show the race.
// Two threads can both see dist == -1 and both add the vertex. Distances still
// come out right (both write the same level), but work is duplicated.
BfsResult bfs_racy(Scheduler& sched, const Graph& g, int source, int grain);
