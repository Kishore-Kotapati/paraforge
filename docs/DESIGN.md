# ParaForge design notes

Plain explanations of how each part works, where the important ideas live in
the code, the real bugs hit while building it, and what the experiments show.

---

## 1. The runtime (`runtime/`)

### Main pieces

| Piece | Where | What it does |
|---|---|---|
| `Scheduler` | scheduler.h | Starts N worker threads, owns one queue per worker |
| `TaskGroup` | scheduler.h | Counts unfinished tasks so a parent can wait for them |
| `spawn(group, fn)` | scheduler.cpp | Adds a task to a queue (which queue depends on the policy) |
| `wait(group)` | scheduler.cpp | Returns when all tasks of the group are done |
| `parallel_for` | scheduler.cpp | Splits a range into tasks |
| `find_task` | scheduler.cpp | The policy itself: where a worker looks for its next task |
| `Trace` | trace.h/.cpp | Per-worker counters: tasks, steals, idle time, nesting depth |

### The three policies, in `find_task`

- **Static**: a worker only looks in its own queue. `parallel_for` puts exactly
  one equal chunk in each queue before anything runs.
- **Shared**: everyone uses queue 0. One lock for all workers.
- **Stealing**: look in your own queue first (newest task, still warm in the
  cache). If empty, pick a random other worker and take its *oldest* task.
  The oldest task is usually the biggest piece of work, so one steal brings a
  lot of work.

### Why a waiting worker runs other tasks

If a worker blocked in `wait()`, its children might be sitting in its own
queue with nobody to run them, and everything would stop (deadlock). So a
worker that waits keeps running tasks until its group is done. A thread from
outside the pool (like `main`) just sleeps on a condition variable instead.

### Concepts, with the exact place in the code

**Shared state and locking.** Each queue is a `std::deque` guarded by its own
`std::mutex` (`WorkQueue`). The shared policy has one lock for everyone; the
stealing policy spreads the locking over N queues, which is why it suffers less
when tasks are tiny.

**Atomics instead of locks for counters.** `TaskGroup::pending_` is a
`std::atomic<int64_t>`. A task finishing does `fetch_sub`, a waiter reads it.

**Memory ordering.** The waiter reads `pending_` with `acquire`; the task
lowers it with `acq_rel`. That pair guarantees that everything a task wrote
(say, its part of a matrix) is visible to the waiter once it sees zero.

**Lost wake-ups.** A sleeping worker must not miss a new task. `push_to` raises
`queued_` and then reads `sleepers_`; a worker going to sleep raises
`sleepers_` and then reads `queued_`. With sequentially consistent atomics,
at least one side sees the other, so somebody always notices the task.

**False sharing.** `WorkerStats` and `WorkQueue` are `alignas(64)`, so each sits
on its own cache line. Without that, two workers updating neighbouring
counters would keep invalidating each other's cache line.

**Exceptions across threads.** If a task throws, `run_task` stores the first
exception in the group and `wait()` rethrows it on the waiting thread. That is
how `10 / 0` inside a `par` branch becomes a normal error message.

---

## 2. The workloads (`workloads/`)

Each one has a sequential baseline, a parallel version, and a check that both
give the same answer.

### Matrix (`matrix.cpp`)
- `multiply_rows` is shared by all versions, so results are bit-identical.
- Each task writes only its own rows of the output, so no locks are needed.
- `multiply_naive` (i-j-k loop order) vs `multiply_seq` (i-k-j): same maths,
  but i-k-j reads memory in order. Measured: 2.6x to 4.6x faster across runs. Cache behaviour
  matters before threads even start.
- `multiply_adaptive` runs sequentially below a size threshold.
  `bench matrix-sizes` shows where parallel starts to pay off: at 8 threads
  on this machine, 16x16 runs at 0.20x and 32x32 at 0.63x, while 64x64 and up win.

### BFS (`graph.cpp`)
- Level by level: all vertices at distance d are expanded in parallel, then
  the next level starts.
- **The race**: two threads can find the same unvisited vertex at the same
  time. `bfs_par` fixes it with `compare_exchange_strong`: only one thread can
  change -1 to the new distance. `bfs_racy` does "load, check, store" and
  measurably adds some vertices twice.
- The racy version still uses atomic loads and stores, so it is a *logic* race
  (a check-then-act gap), not undefined behaviour. That keeps it safe to run
  and ThreadSanitizer-clean, while still showing the problem.
- Each chunk collects new vertices in a local vector and merges once under a
  lock: one lock per chunk instead of one per vertex.

### Chess (`chess.cpp`)
- 0x88 board: square = rank * 16 + file. `(sq & 0x88) == 0` checks "on the
  board" in one operation.
- Copy-make: `make_move` returns a new position and never changes the old one.
  So every task has its own board and nothing chess-related is shared.
- `perft` counts positions and is compared against known numbers
  (20, 400, 8902, 197281). That is the standard test for move generators.
- `search_par` ("young brothers wait" at the root): search the best-ordered
  move alone first to get a good score bound, then search the remaining moves
  in parallel. Each task starts from the best score known so far
  (`best_score`, an atomic updated with a compare-and-swap loop).
- The score always matches the sequential search. The best move can differ
  when two moves score the same, because a move searched with a bound equal
  to its own value cannot prove it is equal (it reports "not better").

### Swarm (`swarm.cpp`)
- Double buffering: read old positions, write new ones to a second array, swap.
  No agent ever reads a half-updated neighbour, so results are **bit-identical**
  for every thread count and policy (checked by hashing all positions).
- A uniform grid finds neighbours, so each agent checks nearby agents only.
- `parallel_for` returning acts as the barrier between steps.

---

## 3. The language (`lang/`)

PFL: integers, functions, pairs, `let`, `let rec`, `if`, and `par(a, b)`.

```
let rec fib n =
  if n < 2 then n
  else let (a, b) = par(fib (n - 1), fib (n - 2)) in a + b
in fib 25
```

- **Lexer** (`lexer.cpp`): text to tokens. Two-character operators are checked
  first, so `<=` is not read as `<` then `=`.
- **Parser** (`parser.cpp`): recursive descent, one function per precedence
  level (compare, sum, term, unary, application, atom). Multi-argument
  functions become nested one-argument functions (currying).
- **Evaluator** (`eval.cpp`): walks the tree. `par` spawns the left side as a
  task, evaluates the right side itself, then waits.

**Why `par` needs no locks.** Values never change after creation and
environments are immutable linked lists (a new binding is a new node in
front; old lists are shared, never edited). Two branches can read the same
environment at the same time with nothing to protect.

**The cost of that in C++.** Sharing is done with `std::shared_ptr`, whose
reference count is atomic. Every variable lookup and function call copies a
`shared_ptr`, and all threads touch the counts of the same shared values
(for example the `fib` closure itself). This is a likely reason why PFL only
reaches about 1.2x on 2 cores while the chess search reaches 1.7-1.9x.
*This is a hypothesis, not yet measured.* A way to test it: run two
independent copies of the program in two threads with no shared values and
see if the speedup jumps.

**`let rec` without a memory leak.** A recursive closure would normally need
an environment that contains itself, which makes a `shared_ptr` cycle that is
never freed. Instead the closure stores its own name (`self_name`) and binds
itself into the environment only when called.

**The cutoff.** Only the first `cutoff` levels of nested `par` create tasks;
deeper ones run inline. `bench lang-cutoff` measures this trade-off.

---

## 4. Bugs found while building it

1. **Stack overflow with many small tasks.** With `par` cutoff 16 the
   benchmark crashed with a segmentation fault. Raising the stack limit
   (`ulimit -s 65536`) made it pass, which pointed to the stack. Cause: a
   waiting worker runs other tasks *inside* its wait; those can wait too and
   run more tasks, so the stack keeps growing. With the shared queue, the
   waiting thread picks whatever task is newest in the global queue, often
   unrelated work, so this nesting gets very deep.
   *Fix*: worker threads are now created with `pthread` and a 64 MB stack
   (only touched pages use memory), outside threads sleep instead of helping,
   and the trace records the deepest nesting. The measurements then showed
   the cause directly: up to 4,469 nested tasks with the shared queue versus
   at most 27 with work stealing (where a waiter usually pops its own child).

2. **Parallel chess faster than sequential on 1 thread.** With one worker,
   the parallel search visited ~30% fewer nodes than the sequential one, which
   made the speedup look better than it was. Cause: tasks were spawned in
   move order and the worker pops newest first, so it searched the moves
   backwards, which happened to prune better in that position. *Fix*: spawn
   in reverse so a single worker searches in the same order as the sequential
   code. Now 1-thread node counts match exactly (search_overhead = 1.00).

3. **Shared variable written by two threads.** An early `search_par` wrote
   `result.score` from the main thread after the tasks had already started
   writing it. *Fix*: the tasks' best score lives in its own variable guarded
   by the same mutex as the best move.

4. **Benchmark noise bigger than the effect.** On this VM, running the same
   single-threaded matrix multiply six times gave 68 to 164 ms. Comparing a
   parallel median against a sequential median measured minutes earlier gave
   impossible results (1-thread "speedup" of 1.6x). *Fix*: take the fastest of
   7 runs (interference only ever slows a run down), and time the sequential
   version again right next to each parallel configuration.

5. **Waiter destroying the group too early** (caught while reviewing the
   design, before it caused a crash). When a waiter can sleep on a
   condition variable, the last task has to notify it. If the task lowered the
   counter and *then* notified, the waiter could see zero, return, and destroy
   the group (it lives on the waiter's stack) before the notify, which would
   touch freed memory. *Fix*: the counter is lowered under the group's mutex,
   and a waiter takes the same mutex before leaving.

All tests also pass under ThreadSanitizer (`make tsan`), with no reports.

---

## 5. What the experiments show (2-vCPU VM)

See `results/bench_output.txt`, the charts in `results/`, and
`results/report.txt` (rule-based findings).

- **Even work scales.** Matrix 1.95x, chess middlegame 1.87x on 2 cores.
- **Work stealing is the safest default.** It was best or close to best on
  every workload, and it never collapsed when tasks got tiny.
- **Static only wins in theory.** With even work it should tie, but it cannot
  react when one core gets slowed down by something else (swarm: 1.22x static
  vs 1.58x stealing at 2 threads).
- **Thread safety has a price.** Parallel BFS on 1 thread is about 2x slower
  than plain BFS (atomics, merging, barriers). On 2 cores it never catches up.
  The trace shows both workers busy the whole time, so "busy" is not the same
  as "useful".
- **Granularity matters.** Too few tasks: idle workers. Too many: overhead,
  and with the shared queue, deep nesting and a big slowdown (0.40x).
- **Parallel search does extra work.** Node counts rise with more threads (up
  to 1.29x at 8 threads) because later moves start with a weaker bound.
- **More threads than cores never helped here**, as expected.

What is *not* shown: anything about 8, 16 or more real cores. That is the
first thing to run on a bigger machine.

---

## 6. The AI part (`tools/`)

- `trace_report.py` is fully deterministic. Fixed rules with visible thresholds
  (idle > 20%, imbalance > 1.5x, nesting > 100, search overhead > 1.2x, ...)
  fire on measured numbers, and every finding prints the numbers behind it. It
  also draws a text timeline of how busy each worker was.
- `ai_explain.py` sends only that report to a language model and asks for a
  likely cause and one experiment per finding. The answer is printed under a
  heading that says it is unverified. Without an API key, nothing changes:
  the measured report is complete on its own.

So: **measured facts** come from the trace and benchmarks; **suggestions**
come from the model and must be checked with an experiment.

---

## 7. OCaml version (`ocaml/pfl.ml`)

The same language in about 230 lines of OCaml, versus about 520 lines of C++
(lexer, parser, evaluator, header; comment lines not counted in either).

| | C++ | OCaml |
|---|---|---|
| Syntax tree | one struct with a `kind` field and generic children | one variant type, one constructor per expression |
| Missing case | compiler warning (with `-Wall`) | compiler warning for every non-exhaustive match |
| Sharing an environment between threads | `shared_ptr`, atomic reference counts | plain immutable list, the GC handles it |
| `par` | task on our own runtime, with cutoff | `Domain.spawn` (an OS thread), with cutoff |
| Cost of a new parallel task | small (a queue push) | large (a new domain), so cutoff must stay low |

Trade-offs, honestly: OCaml makes the language code shorter and safer, and
immutability is the default rather than something to enforce by hand. The C++
version gives full control over scheduling and memory layout, which is the
point of the runtime experiments. A fair next step is OCaml 5 with
Domainslib's work-stealing pool instead of one domain per `par`, then
comparing both on the same machine.

Checked so far: on OCaml 4.14 with a sequential stand-in for `Domain`
(`ocaml/domain_stub_ocaml4.ml`) it prints the same results as the C++ version
for all examples. It still needs a real OCaml 5 run.

---

## 8. Ideas for next steps

1. Lock-free Chase-Lev deque in place of the mutex queues; measure steal cost.
2. Run everything on a machine with 8+ cores.
3. Young-brothers-wait at every level of the chess tree, not just the root.
4. Test the `shared_ptr` hypothesis for PFL (see section 3).
5. Direction-optimizing BFS (switch to bottom-up on big frontiers).
6. OCaml 5 + Domainslib comparison on the same workloads.
