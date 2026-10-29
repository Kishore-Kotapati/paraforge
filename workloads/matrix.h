// Dense matrix multiplication: the "even work" workload.
// Every row costs the same, so this is where static splitting should do well.
#pragma once

#include <cstdint>
#include <vector>

class Scheduler;

struct Matrix {
    int n = 0;
    std::vector<double> data;   // row-major, n*n

    Matrix() = default;
    explicit Matrix(int size) : n(size), data((size_t)size * size, 0.0) {}
    double& at(int r, int c) { return data[(size_t)r * n + c]; }
    double at(int r, int c) const { return data[(size_t)r * n + c]; }
};

Matrix random_matrix(int n, uint32_t seed);

// i-j-k order: walks B down a column, so it misses the cache a lot.
Matrix multiply_naive(const Matrix& a, const Matrix& b);
// i-k-j order: walks B along a row, cache friendly. This is the baseline.
Matrix multiply_seq(const Matrix& a, const Matrix& b);
// Rows are split across workers, `grain_rows` rows per task.
Matrix multiply_par(Scheduler& sched, const Matrix& a, const Matrix& b, int grain_rows);
// Runs sequentially below `threshold`, where thread overhead would win.
Matrix multiply_adaptive(Scheduler& sched, const Matrix& a, const Matrix& b, int threshold);

double max_abs_diff(const Matrix& a, const Matrix& b);
