#include "graph.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "../runtime/scheduler.h"

namespace {

uint32_t lcg(uint32_t& x) {
    x = x * 1664525u + 1013904223u;
    return x >> 1;
}

// Builds CSR from an undirected edge list (each edge stored both ways).
Graph build_csr(int n, const std::vector<std::pair<int, int>>& edges) {
    Graph g;
    g.num_vertices = n;
    g.offset.assign(n + 1, 0);
    for (auto& e : edges) {
        g.offset[e.first + 1]++;
        g.offset[e.second + 1]++;
    }
    for (int v = 0; v < n; v++) g.offset[v + 1] += g.offset[v];
    g.adj.resize(g.offset[n]);
    std::vector<int64_t> fill(g.offset.begin(), g.offset.end() - 1);
    for (auto& e : edges) {
        g.adj[fill[e.first]++] = e.second;
        g.adj[fill[e.second]++] = e.first;
    }
    return g;
}

}  // namespace

Graph random_graph(int n, int avg_degree, uint32_t seed) {
    std::vector<std::pair<int, int>> edges;
    uint32_t x = seed;
    int64_t m = (int64_t)n * avg_degree / 2;
    for (int64_t i = 0; i < m; i++) {
        int a = lcg(x) % n, b = lcg(x) % n;
        if (a != b) edges.push_back({a, b});
    }
    return build_csr(n, edges);
}

Graph skewed_graph(int n, int avg_degree, uint32_t seed) {
    std::vector<std::pair<int, int>> edges;
    uint32_t x = seed;
    int64_t m = (int64_t)n * avg_degree / 2;
    for (int64_t i = 0; i < m; i++) {
        int a = lcg(x) % n;
        // Cubing a uniform number in [0,1) pushes targets toward low ids,
        // so vertex 0, 1, 2... end up with thousands of neighbours.
        double u = (double)(lcg(x) % 1000000) / 1000000.0;
        int b = (int)(u * u * u * n);
        if (a != b) edges.push_back({a, b});
    }
    return build_csr(n, edges);
}

Graph grid_graph(int width, int height) {
    std::vector<std::pair<int, int>> edges;
    for (int r = 0; r < height; r++)
        for (int c = 0; c < width; c++) {
            int v = r * width + c;
            if (c + 1 < width) edges.push_back({v, v + 1});
            if (r + 1 < height) edges.push_back({v, v + width});
        }
    return build_csr(width * height, edges);
}

Graph make_graph(const std::string& kind, int n, uint32_t seed) {
    if (kind == "random") return random_graph(n, 8, seed);
    if (kind == "skewed") return skewed_graph(n, 8, seed);
    if (kind == "grid") {
        int side = 1;
        while ((int64_t)side * side < n) side++;
        return grid_graph(side, side);
    }
    throw std::runtime_error("unknown graph kind: " + kind);
}

std::vector<int> bfs_seq(const Graph& g, int source) {
    std::vector<int> dist(g.num_vertices, -1);
    std::vector<int> frontier{source}, next;
    dist[source] = 0;
    for (int level = 0; !frontier.empty(); level++) {
        next.clear();
        for (int u : frontier)
            for (int64_t e = g.offset[u]; e < g.offset[u + 1]; e++) {
                int v = g.adj[e];
                if (dist[v] == -1) {
                    dist[v] = level + 1;
                    next.push_back(v);
                }
            }
        frontier.swap(next);
    }
    return dist;
}

// Level-by-level parallel BFS. `use_cas` picks the correct or the racy claim.
static BfsResult bfs_levels(Scheduler& sched, const Graph& g, int source, int grain,
                            bool use_cas) {
    int n = g.num_vertices;
    // std::atomic is not copyable, so a plain array instead of a vector.
    std::unique_ptr<std::atomic<int>[]> dist(new std::atomic<int>[n]);
    for (int i = 0; i < n; i++) dist[i].store(-1, std::memory_order_relaxed);
    dist[source] = 0;

    std::vector<int> frontier{source}, next;
    std::mutex next_mutex;
    BfsResult result;

    for (int level = 0; !frontier.empty(); level++) {
        next.clear();
        sched.parallel_for(0, (int64_t)frontier.size(), grain, [&](int64_t lo, int64_t hi) {
            // Collect locally and merge once, so the lock is taken once per
            // chunk instead of once per vertex.
            std::vector<int> local;
            for (int64_t i = lo; i < hi; i++) {
                int u = frontier[i];
                for (int64_t e = g.offset[u]; e < g.offset[u + 1]; e++) {
                    int v = g.adj[e];
                    if (use_cas) {
                        int expected = -1;
                        // Only one thread can change -1 to level+1.
                        if (dist[v].compare_exchange_strong(expected, level + 1,
                                                            std::memory_order_relaxed))
                            local.push_back(v);
                    } else {
                        // Gap between the load and the store: another thread
                        // can claim v in between.
                        if (dist[v].load(std::memory_order_relaxed) == -1) {
                            dist[v].store(level + 1, std::memory_order_relaxed);
                            local.push_back(v);
                        }
                    }
                }
            }
            std::lock_guard<std::mutex> lock(next_mutex);
            next.insert(next.end(), local.begin(), local.end());
        });

        if (!use_cas) {
            // Count and remove duplicates so the next level stays the same size.
            size_t before = next.size();
            std::sort(next.begin(), next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            result.duplicate_visits += (int64_t)(before - next.size());
        }
        frontier.swap(next);
    }

    result.dist.resize(n);
    for (int i = 0; i < n; i++) result.dist[i] = dist[i].load(std::memory_order_relaxed);
    return result;
}

BfsResult bfs_par(Scheduler& sched, const Graph& g, int source, int grain) {
    return bfs_levels(sched, g, source, grain, true);
}

BfsResult bfs_racy(Scheduler& sched, const Graph& g, int source, int grain) {
    return bfs_levels(sched, g, source, grain, false);
}
