// Records what each worker thread did: tasks run, steals, idle time.
//
// Each counter is written only by its own worker, but the main thread reads
// them while workers are still alive, so they are atomics (relaxed order is
// enough: we only want the numbers, not ordering between them).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

enum class EventType { TaskStart, TaskEnd, Steal };

struct TraceEvent {
    EventType type;
    int64_t time_ns;   // since trace creation
    int victim;        // only used for Steal, -1 otherwise
};

// alignas(64) puts each worker's counters on its own cache line.
// Without it, two workers updating neighbouring counters keep invalidating
// each other's cache line (false sharing).
struct alignas(64) WorkerStats {
    std::atomic<int64_t> tasks_run{0};
    std::atomic<int64_t> steals{0};
    std::atomic<int64_t> failed_steals{0};
    std::atomic<int64_t> idle_ns{0};   // looking for work and finding none
    std::atomic<int64_t> max_depth{0}; // deepest nesting of tasks run inside waits
    std::vector<TraceEvent> events;    // only touched while tasks run
};

// Totals at one moment. Subtracting two snapshots gives the numbers for one run.
struct TraceSnapshot {
    int64_t tasks = 0, steals = 0, failed_steals = 0, idle_ns = 0;
    int64_t max_depth = 0;   // since the scheduler started, not per run
    std::vector<int64_t> tasks_per_worker;
};

class Trace {
public:
    explicit Trace(int num_workers, bool keep_events = false);

    void task_start(int worker);
    void task_end(int worker);
    void steal(int worker, int victim);
    void failed_steal(int worker);
    void add_idle(int worker, int64_t ns);
    void note_depth(int worker, int64_t depth);

    int64_t now_ns() const;
    TraceSnapshot snapshot() const;
    void clear_events();   // call only when no tasks are running
    bool write_events_csv(const std::string& path) const;
    int num_workers() const { return (int)stats_.size(); }

private:
    static void bump(std::atomic<int64_t>& x, int64_t v) {
        // single writer, so load + store is safe and cheaper than fetch_add
        x.store(x.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
    }

    std::vector<WorkerStats> stats_;
    bool keep_events_;
    std::chrono::steady_clock::time_point start_;
};

// What one measured run looked like.
struct RunStats {
    double wall_ms = 0;
    int64_t tasks = 0;
    int64_t steals = 0;
    double idle_fraction = 0;   // share of worker time spent idle, 0..1
    double imbalance = 0;       // max tasks on one worker / average
    int64_t max_depth = 0;      // deepest task nesting seen on any worker
};

RunStats diff_stats(const TraceSnapshot& before, const TraceSnapshot& after,
                    double wall_ms, int workers);
