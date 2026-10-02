# =============================================================================================
# Build system for devs-cadmium-everything
#
#   make            build every demo into build/
#   make test       build everything, run every demo (each one self-checks) + the compile-fail
#                   tests + the API coverage audit
#   make run-02_static_greenhouse     build and run a single demo (any name from src/)
#   make clean
#
# Cadmium is a HEADER-ONLY library, so "installing" it just means pointing the compiler at its
# include directory. Override the location with:   make CADMIUM=/path/to/cadmium
# (the default is the sibling checkout used while developing this project).
# =============================================================================================

CADMIUM ?= ../Cadmium-Simulation-Environment/cadmium
CXX     ?= g++

# Cadmium (and its bundled nlohmann json) are -isystem so their own warnings stay quiet,
# while OUR code is compiled with -Wall -Wextra.
INCLUDES  := -Iinclude -isystem $(CADMIUM)/include -isystem $(CADMIUM)/json/single_include
CXXFLAGS  ?= -O1 -g
CXXFLAGS  += -std=c++17 -Wall -Wextra -pthread $(INCLUDES)

# Optional engine modes of the DYNAMIC engine (see README "Concurrency"):
#   concurrent: Cadmium's Boost.Thread thread-pool mode. The three BOOST_THREAD_* macros are
#               required by Boost's executors header (basic_thread_pool.hpp); Cadmium's README
#               only mentions CADMIUM_EXECUTE_CONCURRENT.
#   openmp:     Cadmium's OpenMP mode (CPU_PARALLEL).
CONCURRENT_FLAGS := -DCADMIUM_EXECUTE_CONCURRENT -DBOOST_THREAD_PROVIDES_EXECUTORS \
                    -DBOOST_THREAD_PROVIDES_FUTURE_CONTINUATION -DBOOST_THREAD_USES_MOVE
CONCURRENT_LIBS  := -lboost_system -lboost_thread
OPENMP_FLAGS     := -DCPU_PARALLEL -fopenmp

BUILD := build
SRCS  := $(sort $(wildcard src/*.cpp))
BINS  := $(patsubst src/%.cpp,$(BUILD)/%,$(SRCS))
# the dynamic-engine demo is additionally built in its two parallel flavours
VARIANTS := $(BUILD)/03_dynamic_greenhouse_concurrent $(BUILD)/03_dynamic_greenhouse_openmp
HEADERS := $(shell find include -name '*.hpp')

.PHONY: all test clean compile-fail coverage
all: $(BINS) $(VARIANTS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: src/%.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/03_dynamic_greenhouse_concurrent: src/03_dynamic_greenhouse.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CONCURRENT_FLAGS) $< -o $@ $(CONCURRENT_LIBS)

$(BUILD)/03_dynamic_greenhouse_openmp: src/03_dynamic_greenhouse.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(OPENMP_FLAGS) $< -o $@

run-%: $(BUILD)/%
	./$(BUILD)/$*

# Every demo is also a test: it CHECKs its results and exits non-zero on failure. In addition the
# three execution modes of the dynamic engine (sequential / Boost.Thread / OpenMP) must produce
# byte-identical canonical traces.
test: all compile-fail coverage
	@mkdir -p $(BUILD)/out
	@fail=0; for b in $(BINS) $(VARIANTS); do \
	  echo "################ $$b"; ./$$b > $(BUILD)/out/$$(basename $$b).log 2>&1 && tail -1 $(BUILD)/out/$$(basename $$b).log || { echo "FAILED: $$b (see $(BUILD)/out/$$(basename $$b).log)"; fail=1; }; \
	done; \
	echo "################ trace equality across the three execution modes"; \
	./$(BUILD)/03_dynamic_greenhouse            --dump $(BUILD)/out/trace_sequential.txt > /dev/null 2>&1; \
	./$(BUILD)/03_dynamic_greenhouse_concurrent --dump $(BUILD)/out/trace_concurrent.txt > /dev/null 2>&1; \
	./$(BUILD)/03_dynamic_greenhouse_openmp     --dump $(BUILD)/out/trace_openmp.txt     > /dev/null 2>&1; \
	if cmp -s $(BUILD)/out/trace_sequential.txt $(BUILD)/out/trace_concurrent.txt && cmp -s $(BUILD)/out/trace_sequential.txt $(BUILD)/out/trace_openmp.txt; then \
	  echo "traces identical: sequential == concurrent == openmp ($$(wc -l < $(BUILD)/out/trace_sequential.txt) events)"; \
	else echo "FAILED: the execution modes produced different traces"; fail=1; fi; \
	if [ $$fail -ne 0 ]; then echo "SOME TESTS FAILED"; exit 1; else echo "ALL TESTS PASSED"; fi

compile-fail:
	@CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)" tests/run_compile_fail.sh

coverage:
	@CADMIUM="$(CADMIUM)" tools/coverage_check.sh

clean:
	rm -rf $(BUILD)

