# MiniDB — HeapHackers (Track B: MVCC)
# Requires a C++17 compiler with pthreads.

CXX      ?= c++
# -MMD -MP emit .d files so a changed header recompiles every .cpp that includes
# it (without this, header edits leave stale objects and ABI mismatches).
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread -Isrc -MMD -MP
BUILD    := build

# All engine sources except the two executables' entry points.
LIB_SRCS := $(filter-out src/shell.cpp,$(wildcard src/*.cpp)) \
            $(wildcard src/*/*.cpp)
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(LIB_SRCS))

.PHONY: all clean test bench
all: minidb bench_mvcc bench_engine

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

minidb: $(LIB_OBJS) $(BUILD)/shell.o
	$(CXX) $(CXXFLAGS) $^ -o $@

# The concurrency benchmark links the engine objects it needs plus its own main.
bench_mvcc: bench/bench_mvcc.cpp $(BUILD)/concurrency/lock_manager.o
	$(CXX) $(CXXFLAGS) $^ -o $@

# End-to-end engine benchmark (MVCC vs 2PL through the real storage stack).
bench_engine: bench/bench_engine.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

# Engine-level MVCC tests (links the full library, its own entry point).
$(BUILD)/test_engine: tests/test_engine.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: minidb $(BUILD)/test_engine
	@bash tests/run_tests.sh

clean:
	rm -rf $(BUILD) minidb bench_mvcc bench_engine *.d *.dSYM

# Pull in auto-generated header dependency files.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
