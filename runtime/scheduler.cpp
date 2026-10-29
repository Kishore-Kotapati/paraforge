#include "scheduler.h"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace {

// Which scheduler and worker slot the current thread belongs to.
struct ThreadInfo {
    const void* owner = nullptr;
    int id = -1;
};
thread_local ThreadInfo tl_info;

// How many tasks are running inside each other on this thread. A waiting
// worker runs other tasks, and those can wait too, so this can grow.
thread_local int tl_depth = 0;

// Small per-thread random generator for picking steal victims.
thread_local uint32_t tl_rng = 2463534242u;
uint32_t next_random() {
    tl_rng ^= tl_rng << 13;
    tl_rng ^= tl_rng >> 17;
    tl_rng ^= tl_rng << 5;
    return tl_rng;
}

constexpr int kSpinsBeforeSleep = 64;

// Workers get a large stack because nested waits stack tasks on top of each
// other. Only the pages actually touched use memory.
constexpr size_t kWorkerStackBytes = 64u << 20;

}  // namespace

const char* policy_name(Policy p) {
    switch (p) {
        case Policy::Static: return "static";
        case Policy::SharedQueue: return "shared";
        case Policy::WorkStealing: return "stealing";
    }
    return "?";
}

bool parse_policy(const std::string& s, Policy& out) {
    if (s == "static") out = Policy::Static;
    else if (s == "shared") out = Policy::SharedQueue;
    else if (s == "stealing" || s == "ws") out = Policy::WorkStealing;
    else return false;
    return true;
}

void* Scheduler::thread_entry(void* arg) {
    auto* a = static_cast<StartArgs*>(arg);
    a->sched->worker_loop(a->id);
    return nullptr;
}

Scheduler::Scheduler(int num_threads, Policy policy, bool keep_events)
    : num_threads_(num_threads < 1 ? 1 : num_threads),
      policy_(policy),
      trace_(num_threads_, keep_events),
      queues_(num_threads_) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kWorkerStackBytes);   // keeps default if this fails
    for (int i = 0; i < num_threads_; i++) {
        start_args_.push_back(std::make_unique<StartArgs>(StartArgs{this, i}));
        pthread_t t;
        if (pthread_create(&t, &attr, &Scheduler::thread_entry, start_args_.back().get()) != 0) {
            pthread_attr_destroy(&attr);
            stop_ = true;
            for (pthread_t started : threads_) pthread_join(started, nullptr);
            throw std::runtime_error("could not start worker thread");
        }
        threads_.push_back(t);
    }
    pthread_attr_destroy(&attr);
}

Scheduler::~Scheduler() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_cv_.notify_all();
    }
    for (pthread_t t : threads_) pthread_join(t, nullptr);
}

int Scheduler::current_worker() const {
    return tl_info.owner == this ? tl_info.id : -1;
}

void Scheduler::push_to(int queue, Task task) {
    {
        std::lock_guard<std::mutex> lock(queues_[queue].mutex);
        queues_[queue].tasks.push_back(std::move(task));
    }
    queued_++;
    // queued_ is raised before sleepers_ is read; a worker going to sleep
    // raises sleepers_ before reading queued_. With seq_cst atomics one of
    // the two always sees the other, so a wake-up cannot be lost.
    if (sleepers_ > 0) {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_cv_.notify_one();
    }
}

bool Scheduler::pop_from(int queue, bool from_back, Task& out) {
    WorkQueue& q = queues_[queue];
    std::lock_guard<std::mutex> lock(q.mutex);
    if (q.tasks.empty()) return false;
    if (from_back) {
        out = std::move(q.tasks.back());
        q.tasks.pop_back();
    } else {
        out = std::move(q.tasks.front());
        q.tasks.pop_front();
    }
    queued_--;
    return true;
}

bool Scheduler::find_task(int worker, Task& out) {
    switch (policy_) {
        case Policy::SharedQueue:
            // Newest first, the same order a single thread would use.
            return pop_from(0, true, out);

        case Policy::Static:
            // Only your own queue: whatever was assigned to you up front.
            return pop_from(worker, true, out);

        case Policy::WorkStealing:
            // Owner takes the newest task (still warm in cache);
            // thieves take the oldest, which is usually the biggest piece.
            if (pop_from(worker, true, out)) return true;
            for (int attempt = 0; attempt < num_threads_ - 1; attempt++) {
                int victim = (int)(next_random() % num_threads_);
                if (victim == worker) continue;
                if (pop_from(victim, false, out)) {
                    trace_.steal(worker, victim);
                    return true;
                }
                trace_.failed_steal(worker);
            }
            return false;
    }
    return false;
}

void Scheduler::run_task(int worker, Task& task) {
    tl_depth++;
    trace_.note_depth(worker, tl_depth);
    trace_.task_start(worker);
    std::exception_ptr error;
    try {
        task.fn();
    } catch (...) {
        error = std::current_exception();
    }
    trace_.task_end(worker);
    tl_depth--;

    // The count is lowered under the group lock. A waiter that sees zero then
    // takes the same lock, so it cannot destroy the group (it usually lives
    // on the waiter's stack) while we are still notifying it.
    TaskGroup& g = *task.group;
    std::lock_guard<std::mutex> lock(g.mutex_);
    if (error && !g.error_) g.error_ = error;
    if (g.pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) g.done_.notify_all();
}

void Scheduler::spawn(TaskGroup& group, std::function<void()> fn) {
    group.pending_.fetch_add(1, std::memory_order_relaxed);
    int me = current_worker();
    int target = 0;
    switch (policy_) {
        case Policy::SharedQueue: target = 0; break;
        case Policy::WorkStealing: target = me >= 0 ? me : 0; break;
        case Policy::Static: target = (int)(next_static_++ % num_threads_); break;
    }
    push_to(target, Task{std::move(fn), &group});
}

void Scheduler::rethrow_if_failed(TaskGroup& group) {
    std::exception_ptr e;
    {
        std::lock_guard<std::mutex> lock(group.mutex_);
        e = group.error_;
        group.error_ = nullptr;
    }
    if (e) std::rethrow_exception(e);
}

void Scheduler::wait(TaskGroup& group) {
    int me = current_worker();
    if (me < 0) {
        // Outside thread: just sleep until the last task signals.
        std::unique_lock<std::mutex> lock(group.mutex_);
        group.done_.wait(lock, [&] { return group.pending_.load() == 0; });
        lock.unlock();
        rethrow_if_failed(group);
        return;
    }

    // Worker thread: keep running tasks until our group is done, instead of
    // blocking (blocking here could deadlock: our children may sit in our queue).
    int64_t idle_start = -1;
    while (group.pending_.load(std::memory_order_acquire) > 0) {
        Task task;
        if (find_task(me, task)) {
            if (idle_start >= 0) {
                trace_.add_idle(me, trace_.now_ns() - idle_start);
                idle_start = -1;
            }
            run_task(me, task);
        } else {
            if (idle_start < 0) idle_start = trace_.now_ns();
            std::this_thread::yield();
        }
    }
    if (idle_start >= 0) trace_.add_idle(me, trace_.now_ns() - idle_start);
    rethrow_if_failed(group);   // also takes the group lock, see run_task
}

void Scheduler::worker_loop(int worker) {
    tl_info = {this, worker};
    tl_rng = 2463534242u + 7919u * (uint32_t)worker;

    int fails = 0;
    int64_t idle_start = -1;
    while (!stop_) {
        Task task;
        if (find_task(worker, task)) {
            if (idle_start >= 0) {
                trace_.add_idle(worker, trace_.now_ns() - idle_start);
                idle_start = -1;
            }
            fails = 0;
            run_task(worker, task);
            continue;
        }
        if (idle_start < 0) idle_start = trace_.now_ns();
        if (++fails < kSpinsBeforeSleep) {
            std::this_thread::yield();
            continue;
        }
        // Nothing to do for a while: sleep instead of burning a core.
        // Idle time is flushed before sleeping so a long gap between two
        // benchmark runs is not charged to the next run.
        trace_.add_idle(worker, trace_.now_ns() - idle_start);
        sleepers_++;
        {
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(1),
                              [&] { return stop_ || queued_ > 0; });
        }
        sleepers_--;
        idle_start = trace_.now_ns();
        fails = 0;
    }
}

void Scheduler::split_range(TaskGroup& group, int64_t begin, int64_t end, int64_t grain,
                            const std::function<void(int64_t, int64_t)>& body) {
    // Divide and conquer: hand the right half to the scheduler, keep the left.
    while (end - begin > grain) {
        int64_t mid = begin + (end - begin) / 2;
        spawn(group, [this, &group, mid, end, grain, &body] {
            split_range(group, mid, end, grain, body);
        });
        end = mid;
    }
    body(begin, end);
}

void Scheduler::parallel_for(int64_t begin, int64_t end, int64_t grain,
                             const std::function<void(int64_t, int64_t)>& body) {
    if (end <= begin) return;
    if (grain < 1) grain = 1;
    TaskGroup group;

    if (policy_ == Policy::Static) {
        // Exactly one equal chunk per worker, decided before anything runs.
        int64_t len = end - begin;
        for (int i = 0; i < num_threads_; i++) {
            int64_t lo = begin + len * i / num_threads_;
            int64_t hi = begin + len * (i + 1) / num_threads_;
            if (lo >= hi) continue;
            group.pending_.fetch_add(1, std::memory_order_relaxed);
            push_to(i, Task{[&body, lo, hi] { body(lo, hi); }, &group});
        }
    } else {
        spawn(group, [this, &group, begin, end, grain, &body] {
            split_range(group, begin, end, grain, body);
        });
    }
    wait(group);
}
