# Zero-dependency build (make + any C++20 compiler). cpp/CMakeLists.txt is
# kept for people who prefer CMake; results are identical.
CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -Icpp
BUILD    := build

CORE_SRC := cpp/engine/order_book.cpp \
            cpp/engine/matching_engine.cpp \
            cpp/gateway/trading_core.cpp \
            cpp/gateway/server.cpp \
            cpp/gateway/md_publisher.cpp \
            cpp/common/counting_new.cpp
CORE_OBJ := $(patsubst cpp/%.cpp,$(BUILD)/%.o,$(CORE_SRC))
DEPS     := $(CORE_OBJ:.o=.d)

all: $(BUILD)/hft_gateway $(BUILD)/hft_bench $(BUILD)/hft_bench_gw $(BUILD)/hft_tests

$(BUILD)/%.o: cpp/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/hft_gateway: cpp/apps/gateway_main.cpp $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/hft_bench: cpp/apps/bench_engine.cpp $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/hft_bench_gw: cpp/apps/bench_gateway.cpp $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/hft_tests: cpp/tests/test_main.cpp cpp/tests/unit_tests.cpp $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/hft_tests
	./$(BUILD)/hft_tests

bench: $(BUILD)/hft_bench
	./$(BUILD)/hft_bench

clean:
	rm -rf $(BUILD)

.PHONY: all test bench clean
-include $(DEPS)
