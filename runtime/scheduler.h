// A small task runtime with three scheduling policies.
//
//   Static       - work is split evenly up front, one queue per worker, no stealing
//   SharedQueue  - every worker pushes to and pops from one global queue (one lock)
//   WorkStealing - one queue per worker; an idle worker steals from a random victim
//
// Scheduler(4, ...) starts 4 worker threads. The thread that calls spawn/wait
// from outside the pool just sleeps in wait(); workers that wait run other
// tasks meanwhile.
#pragma once

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "trace.h"

enum class Policy { Static, SharedQueue, WorkStealing };

const char* policy_name(Policy p);
bool parse_policy(const std::string& s, Policy& out);

// Counts unfinished tasks so a parent can wait for its children.
class TaskGroup {
public:
    TaskGroup() = default;
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

private:
    friend class Scheduler;
    std::atomic<int64_t> pending_{0};
    std::mutex mutex_;              // guards error_ and the done_ signal
    std::condition_variable done_;  // wakes an outside thread when pending_ hits 0
    std::exception_ptr error_;      // first exception thrown by a task in this group
};

class Scheduler {
public:
    Scheduler(int num_threads, Policy policy, bool keep_events = false);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Queue fn as a task belonging to group.
    void spawn(TaskGroup& group, std::function<void()> fn);

    // Block until every task in group finished, then rethrow the first
    // exception a task in the group threw (if any).
    void wait(TaskGroup& group);

    // Run body(lo, hi) over [begin, end) in pieces of about `grain` items.
    void parallel_for(int64_t begin, int64_t end, int64_t grain,
                      const std::function<void(int64_t, int64_t)>& body);

    // Worker slot of the calling thread, or -1 for threads outside the pool.
    int current_worker() const;
    int num_threads() const { return num_threads_; }
    Policy policy() const { return policy_; }
    Trace& trace() { return trace_; }

private:
    struct Task {
        std::function<void()> fn;
        TaskGroup* group;
    };

    // Each queue sits on its own cache line so locking one does not slow
    // down a neighbour.
    struct alignas(64) WorkQueue {
        std::mutex mutex;
        std::deque<Task> tasks;
    };

    struct StartArgs {
        Scheduler* sched;
        int id;
    };
    static void* thread_entry(void* arg);

    void push_to(int queue, Task task);
    bool pop_from(int queue, bool from_back, Task& out);
    bool find_task(int worker, Task& out);
    void run_task(int worker, Task& task);
    void worker_loop(int worker);
    void split_range(TaskGroup& group, int64_t begin, int64_t end, int64_t grain,
                     const std::function<void(int64_t, int64_t)>& body);
    void rethrow_if_failed(TaskGroup& group);

    int num_threads_;
    Policy policy_;
    Trace trace_;
    std::vector<WorkQueue> queues_;
    std::vector<pthread_t> threads_;
    std::vector<std::unique_ptr<StartArgs>> start_args_;

    std::atomic<bool> stop_{false};
    std::atomic<int64_t> queued_{0};      // tasks sitting in any queue
    std::atomic<int> sleepers_{0};        // workers blocked on wake_cv_
    std::atomic<uint32_t> next_static_{0};
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
};
