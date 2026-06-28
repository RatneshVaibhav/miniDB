# MiniDB build. No external dependencies — just g++ and make.
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread -Isrc -I. -MMD -MP
LDFLAGS  := -pthread

BUILD := build
LIB   := $(BUILD)/libminidb.a

# Library sources: everything under src/ except the CLI entry point.
LIB_SRCS := $(filter-out src/cli/main.cpp,$(shell find src -name '*.cpp'))
LIB_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(LIB_SRCS))

TEST_SRCS := $(shell find tests -name '*.cpp')
TEST_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(TEST_SRCS))

BENCH_SRCS := $(shell find benchmarks -name '*.cpp')
BENCH_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(BENCH_SRCS))

.PHONY: all test bench run condemo clean
all: minidb minidb_tests minidb_bench minidb_condemo

# ---- archive ----
$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $(LIB_OBJS)

# ---- binaries ----
minidb: $(BUILD)/src/cli/main.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB) $(LDFLAGS)

minidb_tests: $(TEST_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(LDFLAGS)

minidb_bench: $(BENCH_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(BENCH_OBJS) $(LIB) $(LDFLAGS)

minidb_condemo: $(BUILD)/demos/concurrency_demo.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB) $(LDFLAGS)

# ---- generic compile rule ----
$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Header dependency tracking (auto-generated .d files from -MMD).
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

test: minidb_tests
	./minidb_tests

bench: minidb_bench
	./minidb_bench

condemo: minidb_condemo
	./minidb_condemo

run: minidb
	./minidb

clean:
	rm -rf $(BUILD) minidb minidb_tests minidb_bench minidb_condemo *.db *.wal *.meta minidb_data
