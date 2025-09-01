# Makefile for multi-threaded audio graph system
# Builds the modular audio graph demo with proper C11 threading support

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
TARGET = audiograph
DYLIB_TARGET = libaudiograph.dylib

# Source files for the modular build
SOURCES = main.c graph_nodes.c graph_engine.c graph_api.c graph_edit.c
OBJECTS = $(SOURCES:.c=.o)

# Library source files (exclude main.c)
LIB_SOURCES = graph_nodes.c graph_engine.c graph_api.c graph_edit.c
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)

# Header dependencies
HEADERS = graph_types.h graph_nodes.h graph_engine.h graph_api.h graph_edit.h

# Default target
all: $(TARGET) $(DYLIB_TARGET)

# Build the main executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS)

# Build dynamic library for Swift integration
$(DYLIB_TARGET): $(LIB_OBJECTS)
	$(CC) $(CFLAGS) -dynamiclib -install_name @rpath/libaudiograph.dylib \
		-compatibility_version 1.0 -current_version 1.0 \
		-o $(DYLIB_TARGET) $(LIB_OBJECTS)

# Build object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Debug build with more verbose output and debug symbols
debug: CFLAGS += -g -DDEBUG -O0
debug: $(TARGET)

# Release build with optimizations
release: CFLAGS += -O3 -DNDEBUG
release: $(TARGET)

# Library-only build (just the dylib)
lib: $(DYLIB_TARGET)

# Release library build with optimizations
lib-release: CFLAGS += -O3 -DNDEBUG
lib-release: $(DYLIB_TARGET)

# Run the demo
run: $(TARGET)
	./$(TARGET)

# Clean up build artifacts
clean:
	rm -f $(TARGET) $(DYLIB_TARGET) $(OBJECTS) $(TARGET).dSYM a.out

# Check for memory leaks (macOS)
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# Profile performance (requires Xcode tools on macOS)
profile: $(TARGET)
	instruments -t "Time Profiler" ./$(TARGET)

# Test targets
test: test_mpmc_queue test_live_graph_partial_connections test_disconnect test_graph_edit_queue test_queue_api test_capacity_growth test_simple_teardown test_orphan_comprehensive test_auto_sum test_sum_behavior
	./test_mpmc_queue
	./test_live_graph_partial_connections
	./test_disconnect
	./test_graph_edit_queue
	./test_queue_api
	./test_capacity_growth
	./test_simple_teardown
	./test_orphan_comprehensive
	./test_auto_sum
	./test_sum_behavior

# Build MPMC queue unit tests
test_mpmc_queue: test_mpmc_queue.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o
	$(CC) $(CFLAGS) -o test_mpmc_queue test_mpmc_queue.c graph_engine.o graph_nodes.o graph_edit.o

# Build live graph partial connections test (orphaned nodes test)
test_live_graph_partial_connections: test_live_graph_partial_connections.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o
	$(CC) $(CFLAGS) -o test_live_graph_partial_connections test_live_graph_partial_connections.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o

# Build disconnect test (port-based disconnect functionality)
test_disconnect: test_disconnect.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o
	$(CC) $(CFLAGS) -o test_disconnect test_disconnect.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o

# Build graph edit queue test (dynamic editing via queue)
test_graph_edit_queue: test_graph_edit_queue.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o
	$(CC) $(CFLAGS) -o test_graph_edit_queue test_graph_edit_queue.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o

# Build queue API test (pre-allocated IDs API)
test_queue_api: test_queue_api.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o
	$(CC) $(CFLAGS) -o test_queue_api test_queue_api.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o

# Build deletion safety test (worker thread safety with node deletion)
test_deletion_safety: test_deletion_safety.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o
	$(CC) $(CFLAGS) -o test_deletion_safety test_deletion_safety.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o

# Build capacity growth test (dynamic node array expansion)
test_capacity_growth: test_capacity_growth.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o
	$(CC) $(CFLAGS) -o test_capacity_growth test_capacity_growth.c graph_engine.o graph_nodes.o graph_edit.o

# Build simple teardown test (basic graph destruction)
test_simple_teardown: test_simple_teardown.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o
	$(CC) $(CFLAGS) -o test_simple_teardown test_simple_teardown.c graph_engine.o graph_nodes.o graph_edit.o

# Build comprehensive orphan test (focused orphan status validation)
test_orphan_comprehensive: test_orphan_comprehensive.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o
	$(CC) $(CFLAGS) -o test_orphan_comprehensive test_orphan_comprehensive.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o

# Build auto-sum test (automatic summing of multiple edges)
test_auto_sum: test_auto_sum.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o
	$(CC) $(CFLAGS) -o test_auto_sum test_auto_sum.c graph_engine.o graph_nodes.o graph_edit.o

# Build sum behavior test (verify actual audio summing)
test_sum_behavior: test_sum_behavior.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o
	$(CC) $(CFLAGS) -o test_sum_behavior test_sum_behavior.c graph_engine.o graph_nodes.o graph_edit.o

# Clean up test artifacts
clean: clean_tests

clean_tests:
	rm -f test_mpmc_queue test_engine_workers test_live_graph_multithreaded test_live_graph_workers test_live_graph_partial_connections test_disconnect test_graph_edit_queue test_queue_api test_deletion_safety test_capacity_growth test_simple_teardown test_orphan_comprehensive test_auto_sum test_sum_behavior

.PHONY: all debug release lib lib-release run clean valgrind profile test clean_tests
