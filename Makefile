CC ?= cc
CFLAGS := -std=c17 -Wall -Wextra -Werror -pedantic -Iinclude
BUILD_DIR := build
TARGET := $(BUILD_DIR)/plant-monitor
TEST_TARGET := $(BUILD_DIR)/test_analysis
TRANSPORT_TEST_TARGET := $(BUILD_DIR)/test_i2c_transport
TRANSPORT_SRCS := src/sensors/i2c_transport.c src/sensors/i2c_mock.c
TRANSPORT_HDRS := include/sensors/i2c_transport.h include/sensors/i2c_mock.h
LINUX_TEST_TARGET := $(BUILD_DIR)/test_i2c_linux
LINUX_SRCS := src/sensors/i2c_transport.c src/sensors/i2c_linux.c
LINUX_HDRS := include/sensors/i2c_transport.h include/sensors/i2c_linux.h \
	src/sensors/i2c_linux_sys.h

.PHONY: all test asan memcheck clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): src/main.c src/analysis.c include/analysis.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/main.c src/analysis.c -o $(TARGET)

$(TEST_TARGET): tests/test_analysis.c src/analysis.c include/analysis.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_analysis.c src/analysis.c -o $(TEST_TARGET)

$(TRANSPORT_TEST_TARGET): tests/test_i2c_transport.c $(TRANSPORT_SRCS) $(TRANSPORT_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_i2c_transport.c $(TRANSPORT_SRCS) -o $(TRANSPORT_TEST_TARGET)

$(LINUX_TEST_TARGET): tests/test_i2c_linux.c $(LINUX_SRCS) $(LINUX_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_i2c_linux.c $(LINUX_SRCS) -o $(LINUX_TEST_TARGET)

test: $(TEST_TARGET) $(TRANSPORT_TEST_TARGET) $(LINUX_TEST_TARGET)
	./$(TEST_TARGET)
	./$(TRANSPORT_TEST_TARGET)
	./$(LINUX_TEST_TARGET)

ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -g

# asan: build and test in $(BUILD_DIR)/asan so normal binaries are untouched.
asan:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/asan CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" all test

memcheck: $(TEST_TARGET) $(TRANSPORT_TEST_TARGET) $(LINUX_TEST_TARGET)
	@if command -v valgrind >/dev/null 2>&1; then \
		valgrind --leak-check=full --error-exitcode=1 ./$(TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(TRANSPORT_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(LINUX_TEST_TARGET); \
	else \
		echo "valgrind is not installed"; exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)
