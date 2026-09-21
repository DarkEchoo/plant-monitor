CC ?= cc
CFLAGS := -std=c17 -Wall -Wextra -Werror -pedantic -Iinclude
BUILD_DIR := build
TARGET := $(BUILD_DIR)/plant-monitor
TEST_TARGET := $(BUILD_DIR)/test_analysis

.PHONY: all test asan memcheck clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): src/main.c src/analysis.c include/analysis.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/main.c src/analysis.c -o $(TARGET)

$(TEST_TARGET): tests/test_analysis.c src/analysis.c include/analysis.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_analysis.c src/analysis.c -o $(TEST_TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
asan: clean all test

memcheck: $(TEST_TARGET)
	@if command -v valgrind >/dev/null 2>&1; then \
		valgrind --leak-check=full --error-exitcode=1 ./$(TEST_TARGET); \
	else \
		echo "valgrind is not installed"; exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)
