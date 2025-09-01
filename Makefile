# Makefile for multi-threaded audio graph system
# Builds the modular audio graph demo with proper C11 threading support

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
TARGET = audiograph

# Source files for the modular build
SOURCES = main.c graph_nodes.c graph_engine.c graph_api.c graph_edit.c
OBJECTS = $(SOURCES:.c=.o)

# Header dependencies
HEADERS = graph_types.h graph_nodes.h graph_engine.h graph_api.h graph_edit.h

# Default target
all: $(TARGET)

# Build the main executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS)

# Build object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Debug build with more verbose output and debug symbols
debug: CFLAGS += -g -DDEBUG -O0
debug: $(TARGET)

# Release build with optimizations
release: CFLAGS += -O3 -DNDEBUG
release: $(TARGET)

# Run the demo
run: $(TARGET)
	./$(TARGET)

# Clean up build artifacts
clean:
	rm -f $(TARGET) $(OBJECTS) $(TARGET).dSYM a.out

# Check for memory leaks (macOS)
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# Profile performance (requires Xcode tools on macOS)
profile: $(TARGET)
	instruments -t "Time Profiler" ./$(TARGET)

# Test targets
test: test_mpmc_queue test_live_graph_partial_connections test_disconnect test_graph_edit_queue test_queue_api
	./test_mpmc_queue
	./test_live_graph_partial_connections
	./test_disconnect
	./test_graph_edit_queue
	./test_queue_api

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

# Clean up test artifacts
clean: clean_tests

clean_tests:
	rm -f test_mpmc_queue test_engine_workers test_live_graph_multithreaded test_live_graph_workers test_live_graph_partial_connections test_disconnect test_graph_edit_queue test_queue_api test_deletion_safety

.PHONY: all debug release run clean valgrind profile test clean_tests
