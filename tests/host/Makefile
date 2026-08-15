# Host tests for the OTLP exporters.
#
# The exporters keep their Switch calls behind #ifdef __SWITCH__, so on a
# development machine they compile against the standard library fallbacks and
# every layer below the transport can be exercised without hardware.
#
#   make            build and run
#   make tsan       run under ThreadSanitizer   (races between the exporter's callers)
#   make asan       run under Address/UB sanitizers
#   make all-checks all three, which is what CI runs
#
# Why this is worth having: the exporters are driven from the main thread, a
# detached termination thread, brls::async workers and the render thread at
# once. On the console a race between them presents as the crash under
# investigation. Here it presents as a stack trace, in about a second.

SRC_DIR := ../../app/src/utils
SOURCES := $(SRC_DIR)/OtlpTraceExporter.cpp $(SRC_DIR)/OtlpMetricsExporter.cpp \
           $(SRC_DIR)/ProcessHealth.cpp
TEST    := test_otlp.cpp

CXXFLAGS := -std=c++20 -I$(SRC_DIR) -Wall -Wextra -Wno-unused-parameter -g

.PHONY: all run tsan asan all-checks clean

all: run

run: build/test_otlp
	@./build/test_otlp

build/test_otlp: $(TEST) $(SOURCES)
	@mkdir -p build
	@$(CXX) $(CXXFLAGS) -O1 $(TEST) $(SOURCES) -o $@

# Races here are the ones that matter: the trace exporter's buffer is filled by
# several threads and swapped out by the worker that posts it.
tsan: build/test_otlp_tsan
	@echo "== ThreadSanitizer =="
	@./build/test_otlp_tsan

build/test_otlp_tsan: $(TEST) $(SOURCES)
	@mkdir -p build
	@$(CXX) $(CXXFLAGS) -O1 -fsanitize=thread $(TEST) $(SOURCES) -o $@

asan: build/test_otlp_asan
	@echo "== Address + UndefinedBehavior sanitizers =="
	@./build/test_otlp_asan

build/test_otlp_asan: $(TEST) $(SOURCES)
	@mkdir -p build
	@$(CXX) $(CXXFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(TEST) $(SOURCES) -o $@

all-checks: run asan tsan
	@echo "all host checks passed"

clean:
	@rm -rf build
