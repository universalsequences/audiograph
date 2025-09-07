# Makefile for multi-threaded audio graph system
# Builds the modular audio graph demo with proper C11 threading support

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
TARGET = audiograph
DYLIB_TARGET = libaudiograph.dylib

# Source files for the modular build
SOURCES = main.c graph_nodes.c graph_engine.c graph_api.c graph_edit.c ready_queue.c hot_swap.c
OBJECTS = $(SOURCES:.c=.o)

# Library source files (exclude main.c)
LIB_SOURCES = graph_nodes.c graph_engine.c graph_api.c graph_edit.c ready_queue.c hot_swap.c
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)

# Header dependencies
HEADERS = graph_types.h graph_nodes.h graph_engine.h graph_api.h graph_edit.h hot_swap.h

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
test: tests/test_mpmc_queue tests/test_live_graph_partial_connections tests/test_disconnect tests/test_graph_edit_queue tests/test_queue_api tests/test_capacity_growth tests/test_simple_teardown tests/test_orphan_comprehensive tests/test_auto_sum tests/test_sum_behavior tests/test_hot_swap tests/test_multi_port_routing
	./tests/test_mpmc_queue
	./tests/test_live_graph_partial_connections
	./tests/test_disconnect
	./tests/test_graph_edit_queue
	./tests/test_queue_api
	./tests/test_capacity_growth
	./tests/test_simple_teardown
	./tests/test_orphan_comprehensive
	./tests/test_auto_sum
	./tests/test_sum_behavior
	./tests/test_hot_swap
	./tests/test_multi_port_routing

# Build MPMC queue unit tests
tests/test_mpmc_queue: tests/test_mpmc_queue.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_mpmc_queue tests/test_mpmc_queue.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build live graph partial connections test (orphaned nodes test)
tests/test_live_graph_partial_connections: tests/test_live_graph_partial_connections.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_live_graph_partial_connections tests/test_live_graph_partial_connections.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o

# Build disconnect test (port-based disconnect functionality)
tests/test_disconnect: tests/test_disconnect.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_disconnect tests/test_disconnect.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o

# Build graph edit queue test (dynamic editing via queue)
tests/test_graph_edit_queue: tests/test_graph_edit_queue.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_graph_edit_queue tests/test_graph_edit_queue.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o

# Build queue API test (pre-allocated IDs API)
tests/test_queue_api: tests/test_queue_api.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_queue_api tests/test_queue_api.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o

# Build deletion safety test (worker thread safety with node deletion)
tests/test_deletion_safety: tests/test_deletion_safety.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_deletion_safety tests/test_deletion_safety.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o

# Build capacity growth test (dynamic node array expansion)
tests/test_capacity_growth: tests/test_capacity_growth.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_capacity_growth tests/test_capacity_growth.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build simple teardown test (basic graph destruction)
tests/test_simple_teardown: tests/test_simple_teardown.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_simple_teardown tests/test_simple_teardown.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build comprehensive orphan test (focused orphan status validation)
tests/test_orphan_comprehensive: tests/test_orphan_comprehensive.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_orphan_comprehensive tests/test_orphan_comprehensive.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o

# Build auto-sum test (automatic summing of multiple edges)
tests/test_auto_sum: tests/test_auto_sum.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_auto_sum tests/test_auto_sum.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build sum behavior test (verify actual audio summing)
tests/test_sum_behavior: tests/test_sum_behavior.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_sum_behavior tests/test_sum_behavior.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build hot swap test (verify hot swap functionality)
tests/test_hot_swap: tests/test_hot_swap.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_hot_swap tests/test_hot_swap.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build number node test (verify NUMBER node functionality)
tests/test_number_node: tests/test_number_node.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_number_node tests/test_number_node.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build multi-port routing test (verify multi-output to multi-input connections)
tests/test_multi_port_routing: tests/test_multi_port_routing.c $(HEADERS) graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_multi_port_routing tests/test_multi_port_routing.c graph_engine.o graph_nodes.o graph_edit.o ready_queue.o hot_swap.o

# Build new worker system test (validate block-boundary wake system)
tests/test_new_worker_system: tests/test_new_worker_system.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o
	$(CC) $(CFLAGS) -I. -o tests/test_new_worker_system tests/test_new_worker_system.c graph_engine.o graph_nodes.o graph_api.o graph_edit.o ready_queue.o hot_swap.o

# Clean up test artifacts
clean: clean_tests

clean_tests:
	rm -f tests/test_mpmc_queue tests/test_engine_workers tests/test_live_graph_multithreaded tests/test_live_graph_workers tests/test_live_graph_partial_connections tests/test_disconnect tests/test_graph_edit_queue tests/test_queue_api tests/test_deletion_safety tests/test_capacity_growth tests/test_simple_teardown tests/test_orphan_comprehensive tests/test_auto_sum tests/test_sum_behavior tests/test_engine_workers_debug tests/test_number_node tests/test_orphan_edge_cases tests/test_new_worker_system tests/test_hot_swap tests/test_multi_port_routing
	rm -rf tests/*.dSYM

.PHONY: all debug release lib lib-release run clean valgrind profile test clean_tests
