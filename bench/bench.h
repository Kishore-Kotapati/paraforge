// Benchmark harness: runs a workload sequentially and then under every
// policy and thread count, checks the answer, and records the numbers.
#pragma once

#include <string>
#include <vector>

#include "../runtime/scheduler.h"

struct BenchOptions {
    std::vector<int> threads{1, 2, 4, 8};
    std::vector<Policy> policies{Policy::Static, Policy::SharedQueue, Policy::WorkStealing};
    int size = -1;            // workload specific; -1 = default
    int reps = 5;             // timing = fastest of reps
    std::string csv_path;     // append results here if set
    std::string trace_path;   // write a per-task event trace here if set
};

// workload: matrix, matrix-sizes, graph, chess, swarm, lang, lang-cutoff, all
// Returns 0 if every run produced the correct answer.
int run_bench(const std::string& workload, const BenchOptions& opts);
