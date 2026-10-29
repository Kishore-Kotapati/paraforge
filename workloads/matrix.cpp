#include "matrix.h"

#include <cmath>

#include "../runtime/scheduler.h"

Matrix random_matrix(int n, uint32_t seed) {
    Matrix m(n);
    uint32_t x = seed ? seed : 1;
    for (auto& v : m.data) {
        x = x * 1664525u + 1013904223u;          // simple LCG, same on every machine
        v = (double)(x >> 8) / (1u << 24) - 0.5;  // range [-0.5, 0.5)
    }
    return m;
}

Matrix multiply_naive(const Matrix& a, const Matrix& b) {
    int n = a.n;
    Matrix c(n);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double sum = 0;
            for (int k = 0; k < n; k++) sum += a.at(i, k) * b.at(k, j);
            c.at(i, j) = sum;
        }
    return c;
}

// Computes rows [row_begin, row_end) of c. Shared by all versions so they do
// exactly the same arithmetic and give bit-identical results.
static void multiply_rows(const Matrix& a, const Matrix& b, Matrix& c,
                          int64_t row_begin, int64_t row_end) {
    int n = a.n;
    for (int64_t i = row_begin; i < row_end; i++) {
        double* c_row = &c.data[(size_t)i * n];
        for (int k = 0; k < n; k++) {
            double a_ik = a.at((int)i, k);
            const double* b_row = &b.data[(size_t)k * n];
            for (int j = 0; j < n; j++) c_row[j] += a_ik * b_row[j];
        }
    }
}

Matrix multiply_seq(const Matrix& a, const Matrix& b) {
    Matrix c(a.n);
    multiply_rows(a, b, c, 0, a.n);
    return c;
}

Matrix multiply_par(Scheduler& sched, const Matrix& a, const Matrix& b, int grain_rows) {
    Matrix c(a.n);
    // Each task writes only its own rows of c, so no locking is needed.
    sched.parallel_for(0, a.n, grain_rows, [&](int64_t lo, int64_t hi) {
        multiply_rows(a, b, c, lo, hi);
    });
    return c;
}

Matrix multiply_adaptive(Scheduler& sched, const Matrix& a, const Matrix& b, int threshold) {
    if (a.n < threshold || sched.num_threads() == 1) return multiply_seq(a, b);
    // About 4 tasks per worker leaves room for balancing without tiny tasks.
    int grain = a.n / (4 * sched.num_threads());
    return multiply_par(sched, a, b, grain < 1 ? 1 : grain);
}

double max_abs_diff(const Matrix& a, const Matrix& b) {
    if (a.n != b.n) return INFINITY;
    double worst = 0;
    for (size_t i = 0; i < a.data.size(); i++)
        worst = std::fmax(worst, std::fabs(a.data[i] - b.data[i]));
    return worst;
}
