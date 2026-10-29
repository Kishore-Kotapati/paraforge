#include "trace.h"

#include <algorithm>
#include <fstream>

Trace::Trace(int num_workers, bool keep_events)
    : stats_(num_workers), keep_events_(keep_events),
      start_(std::chrono::steady_clock::now()) {}

int64_t Trace::now_ns() const {
    auto d = std::chrono::steady_clock::now() - start_;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

void Trace::task_start(int worker) {
    if (keep_events_)
        stats_[worker].events.push_back({EventType::TaskStart, now_ns(), -1});
}

void Trace::task_end(int worker) {
    bump(stats_[worker].tasks_run, 1);
    if (keep_events_)
        stats_[worker].events.push_back({EventType::TaskEnd, now_ns(), -1});
}

void Trace::steal(int worker, int victim) {
    bump(stats_[worker].steals, 1);
    if (keep_events_)
        stats_[worker].events.push_back({EventType::Steal, now_ns(), victim});
}

void Trace::failed_steal(int worker) { bump(stats_[worker].failed_steals, 1); }

void Trace::add_idle(int worker, int64_t ns) { bump(stats_[worker].idle_ns, ns); }

void Trace::note_depth(int worker, int64_t depth) {
    auto& d = stats_[worker].max_depth;
    if (depth > d.load(std::memory_order_relaxed)) d.store(depth, std::memory_order_relaxed);
}

TraceSnapshot Trace::snapshot() const {
    TraceSnapshot s;
    for (auto& w : stats_) {
        int64_t t = w.tasks_run.load(std::memory_order_relaxed);
        s.tasks += t;
        s.tasks_per_worker.push_back(t);
        s.steals += w.steals.load(std::memory_order_relaxed);
        s.failed_steals += w.failed_steals.load(std::memory_order_relaxed);
        s.idle_ns += w.idle_ns.load(std::memory_order_relaxed);
        s.max_depth = std::max(s.max_depth, w.max_depth.load(std::memory_order_relaxed));
    }
    return s;
}

void Trace::clear_events() {
    for (auto& w : stats_) w.events.clear();
}

bool Trace::write_events_csv(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "worker,event,time_us,victim\n";
    const char* names[] = {"start", "end", "steal"};
    for (size_t w = 0; w < stats_.size(); w++) {
        for (auto& e : stats_[w].events) {
            out << w << "," << names[(int)e.type] << ","
                << e.time_ns / 1000.0 << "," << e.victim << "\n";
        }
    }
    return true;
}

RunStats diff_stats(const TraceSnapshot& before, const TraceSnapshot& after,
                    double wall_ms, int workers) {
    RunStats r;
    r.wall_ms = wall_ms;
    r.tasks = after.tasks - before.tasks;
    r.steals = after.steals - before.steals;
    r.max_depth = after.max_depth;

    double wall_ns = wall_ms * 1e6;
    if (wall_ns > 0) {
        double idle = (double)(after.idle_ns - before.idle_ns);
        r.idle_fraction = std::clamp(idle / (wall_ns * workers), 0.0, 1.0);
    }

    int64_t max_tasks = 0;
    for (size_t i = 0; i < after.tasks_per_worker.size(); i++) {
        int64_t t = after.tasks_per_worker[i] - before.tasks_per_worker[i];
        max_tasks = std::max(max_tasks, t);
    }
    double avg = r.tasks > 0 ? (double)r.tasks / workers : 0;
    r.imbalance = avg > 0 ? max_tasks / avg : 0;
    return r;
}
