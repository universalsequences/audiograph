# Makefile for multi-threaded audio graph system
# Builds the modular audio graph demo with proper C11 threading support

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
TARGET = audiograph

# Source files for the modular build
SOURCES = main.c graph_nodes.c graph_engine.c graph_api.c
OBJECTS = $(SOURCES:.c=.o)

# Header dependencies
HEADERS = graph_types.h graph_nodes.h graph_engine.h graph_api.h

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
test: test_mpmc_queue test_engine_workers
	./test_mpmc_queue
	./test_engine_workers

# Build MPMC queue unit tests
test_mpmc_queue: test_mpmc_queue.c $(HEADERS) graph_engine.o graph_nodes.o
	$(CC) $(CFLAGS) -o test_mpmc_queue test_mpmc_queue.c graph_engine.o graph_nodes.o

# Build engine worker race condition tests (the real test!)
test_engine_workers: test_engine_workers.c $(HEADERS) graph_engine.o graph_nodes.o graph_api.o
	$(CC) $(CFLAGS) -o test_engine_workers test_engine_workers.c graph_engine.o graph_nodes.o graph_api.o

# Clean up test artifacts
clean: clean_tests

clean_tests:
	rm -f test_mpmc_queue test_livegraph_workers test_engine_workers

.PHONY: all debug release run clean valgrind profile test clean_tests