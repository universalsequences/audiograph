# Makefile for multi-threaded audio graph system
# Builds the graph.c demo with proper C11 threading support

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
TARGET = audiograph
SOURCE = graph.c

# Default target
all: $(TARGET)

# Build the main executable
$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

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
	rm -f $(TARGET) $(TARGET).dSYM a.out

# Check for memory leaks (macOS)
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# Profile performance (requires Xcode tools on macOS)
profile: $(TARGET)
	instruments -t "Time Profiler" ./$(TARGET)

.PHONY: all debug release run clean valgrind profile