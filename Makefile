CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
INCLUDES  = -I.

CORE = runtime/scheduler.cpp runtime/trace.cpp \
       workloads/matrix.cpp workloads/graph.cpp workloads/chess.cpp workloads/swarm.cpp \
       lang/lexer.cpp lang/parser.cpp lang/eval.cpp
HEADERS = $(wildcard runtime/*.h workloads/*.h lang/*.h bench/*.h)

all: paraforge run_tests

paraforge: $(CORE) bench/bench.cpp app/main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CORE) bench/bench.cpp app/main.cpp -o $@

run_tests: $(CORE) tests/tests.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CORE) tests/tests.cpp -o $@

test: run_tests
	./run_tests

# Same tests under ThreadSanitizer: reports data races at run time.
tsan: $(CORE) tests/tests.cpp $(HEADERS)
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(INCLUDES) $(CORE) tests/tests.cpp -o run_tests_tsan
	./run_tests_tsan

bench: paraforge
	mkdir -p results
	rm -f results/results.csv
	./paraforge bench all --csv results/results.csv

gist: gist/parallel_tictactoe.cpp
	$(CXX) $(CXXFLAGS) gist/parallel_tictactoe.cpp -o tictactoe

clean:
	rm -f paraforge run_tests run_tests_tsan tictactoe

.PHONY: all test tsan bench gist clean
