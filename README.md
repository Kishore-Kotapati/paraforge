# ParaForge

A small parallel task runtime written from scratch in C++, plus a tiny functional
language that runs on it, tested with four very different workloads and shown
through an AR chessboard.

**Question it studies:** does the *shape* of the work decide which scheduling
method wins?

| Workload | Shape of the work |
|---|---|
| Matrix multiply | even, predictable |
| BFS on graphs | messy, shared data, many small levels |
| Chess alpha-beta | very uneven tree, parallel search can waste work |
| Swarm simulation | many equal tasks, every step must finish before the next |
| PFL language (`par`) | tasks created by the user's program, any size |

Each one runs under three schedulers, all built here:

- **static**: work split evenly before it starts, no rebalancing
- **shared**: one global queue with one lock
- **stealing**: one queue per worker; idle workers steal from others

## Layout

```
runtime/    scheduler.h/.cpp  (3 policies, spawn/wait, parallel_for)
            trace.h/.cpp      (per-worker counters + event trace)
workloads/  matrix, graph (BFS), chess (move gen + search), swarm
lang/       lexer, parser, evaluator for PFL (par runs on the runtime)
bench/      benchmark harness: every workload x policy x thread count
app/        command line: bench, run, engine, perft
tests/      245 correctness checks (no framework)
tools/      plot.py, trace_report.py (rule based), ai_explain.py (optional AI)
ar/         ar_chess.py: webcam AR board that talks to the engine
ocaml/      pfl.ml: the same language in OCaml 5 with Domains
gist/       parallel_tictactoe.cpp: standalone code sample
examples/   PFL programs
results/    benchmark output, charts, traces, report from one real run
docs/       DESIGN.md: how it works, bugs found, what the results show
```

```
            AR viewer (Python, optional)
                    | text commands (stdin/stdout)
   PFL language   Chess    Graph    Swarm    Matrix
        \           |        |        |        /
   ======== Runtime: static | shared | stealing ========
     Trace  .  Benchmarks  .  Correctness checks  .  Report (+ optional AI)
```

## Build and test (Ubuntu or WSL)

```bash
sudo apt install build-essential python3-pip
make            # builds ./paraforge and ./run_tests
make test       # 245 checks
make tsan       # same tests under ThreadSanitizer (data race detector)
```

Needs g++ 9+ (C++17). Python tools need `pip install matplotlib opencv-contrib-python numpy`.

## Run

```bash
# benchmarks (prints a table, appends to a CSV)
./paraforge bench matrix
./paraforge bench all --threads 1,2,4,8 --reps 7 --csv results/results.csv
./paraforge bench chess --size 6          # size = search depth here
./paraforge bench graph --trace results/trace.csv

# charts + report
python3 tools/plot.py results/results.csv results/
python3 tools/trace_report.py results/results.csv results/trace_bfs-skewed.csv
python3 tools/ai_explain.py results/results.csv   # uses ANTHROPIC_API_KEY if set

# PFL programs
./paraforge run examples/fib.pf --threads 4 --policy stealing --cutoff 10

# chess
./paraforge perft 4
./paraforge engine --threads 4 --depth 4    # then type: move e2e4 / go / board / quit
python3 ar/ar_chess.py --make-marker         # print marker.png, put it on a table
python3 ar/ar_chess.py                       # webcam AR board
python3 ar/ar_chess.py --no-camera           # flat board, click to move

# code sample
make gist && ./tictactoe 4
```

Workloads for `bench`: `matrix`, `matrix-sizes`, `graph`, `chess`, `swarm`, `lang`,
`lang-cutoff`, `all`. `--size` means matrix size, vertex count, search depth,
agent count or fib argument, depending on the workload.

### Sample output

```
$ ./paraforge run examples/fib.pf --threads 2
75025
[94.90 ms, 2 threads, stealing, 1023 par tasks]

$ ./paraforge run examples/error.pf
error: line 2: division by zero
```

## Results (one real run)

Machine: cloud VM, **2 vCPUs** (Intel Xeon 2.1 GHz), Ubuntu 24.04, g++ 13.3, `-O2`.
Each number is the fastest of 7 runs, and speedup is against a sequential run
timed right next to it (this VM is noisy; see docs/DESIGN.md). Full table:
`results/bench_output.txt`. **Run it on your own machine: with more cores the
numbers will be different.**

Best speedup at 2 threads (max possible: 2.0):

| Workload | static | shared | stealing | Answer correct |
|---|---|---|---|---|
| matrix 512x512 | 1.61 | 1.89 | **1.95** | yes |
| chess middlegame, depth 5 | 1.66 | **1.87** | 1.74 | yes |
| swarm, 4000 agents | 1.22 | 1.48 | **1.58** | yes, bit-identical |
| PFL fib 24 | 1.09 | 1.09 | **1.21** | yes |
| BFS random graph | 0.81 | 0.72 | **0.84** | yes |
| BFS grid graph | 0.21 | 0.10 | 0.12 | yes |

What these show, in short (details and graphs in docs/DESIGN.md):

1. **Even work scales**: matrix and chess get close to 2x on 2 cores.
2. **Static splitting is fragile**: with even work it should tie, but when the
   OS or another process slows one core, its fixed share finishes late
   (swarm: 54 ms static vs 42 ms stealing at 2 threads).
3. **Thread-safe BFS is slower than plain BFS here.** With 1 thread the parallel
   version already takes about 2x as long (atomic compare-and-swap on every
   edge, merging frontiers, a barrier per level). 2 cores cannot win that back.
   The grid graph is worst: about 1,400 tiny levels, so overhead dominates.
4. **Too many small tasks hurt, and the shared queue hurts most.** Raising the
   PFL `par` cutoff from 12 to 20 makes the shared queue drop to 0.40x, while
   work stealing only drops to 0.89x. The trace shows why: tasks nest 4,469 deep
   inside waits with the shared queue, but at most 27 deep with stealing.
   This is also the cause of a stack-overflow crash found during development.
5. **The race is real**: the deliberately wrong "check then set" BFS claimed
   8 to 14 vertices twice per run; the compare-and-swap version never does.
6. **Parallel search wastes work**: at 8 threads chess visited up to 1.29x the
   nodes of the sequential search.

## Limitations

- Results come from a 2-core VM. Claims about scaling beyond 2 cores are untested.
- Queues use a mutex, not a lock-free deque (Chase-Lev). Simpler to get right,
  but slower under heavy stealing.
- Chess has no castling and no en passant, and a very simple evaluation, so the
  engine plays weakly. It exists as a workload; perft is exact up to depth 4.
- Parallel chess splits only at the root.
- PFL has integers, pairs and functions only. No lists, no type checker,
  deep recursion can overflow the stack.
- The OCaml version was checked on OCaml 4.14 with a sequential stand-in for
  `Domain` (same answers as C++). It has not been timed on a real OCaml 5 install.
- The AR viewer uses a printed marker and mouse clicks, not hand tracking.
- The AI step only explains measured findings. It is optional, and its output
  is labelled as unverified suggestions.

## Related work worth reading

- R. Blumofe, C. Leiserson. *Scheduling Multithreaded Computations by Work
  Stealing*. JACM 46(5), 1999.
- D. Chase, Y. Lev. *Dynamic Circular Work-Stealing Deque*. SPAA 2005.
- R. Feldmann et al. *Distributed Game-Tree Search* (Young Brothers Wait), 1989-1994.
- K. Sivaramakrishnan et al. *Retrofitting Parallelism onto OCaml*. ICFP 2020.
- OCaml `Domainslib` (task pool with work stealing for OCaml 5).

These ideas are established. This project's own part is the implementation,
the side-by-side measurements on five different work shapes, and the trace
tools that explain the numbers.
