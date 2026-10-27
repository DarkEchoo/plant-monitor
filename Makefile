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
BH1750_TEST_TARGET := $(BUILD_DIR)/test_bh1750
BH1750_SRCS := src/sensors/bh1750.c $(TRANSPORT_SRCS)
BH1750_HDRS := include/sensors/bh1750.h $(TRANSPORT_HDRS)
SOIL_TEST_TARGET := $(BUILD_DIR)/test_seesaw_soil
SOIL_SRCS := src/sensors/seesaw_soil.c $(TRANSPORT_SRCS)
SOIL_HDRS := include/sensors/seesaw_soil.h $(TRANSPORT_HDRS)
DIAG_TARGET := $(BUILD_DIR)/sensor-diag
DIAG_TEST_TARGET := $(BUILD_DIR)/test_sensor_diag
DIAG_SRCS := src/sensors/sensor_diag.c src/sensors/bh1750.c \
	src/sensors/seesaw_soil.c src/sensors/i2c_transport.c
DIAG_HDRS := include/sensors/sensor_diag.h include/sensors/bh1750.h \
	include/sensors/seesaw_soil.h include/sensors/i2c_transport.h
CAPTURE_TEST_TARGET := $(BUILD_DIR)/test_capture
CAPTURE_SRCS := src/sensors/capture.c src/sensors/bh1750.c \
	src/sensors/seesaw_soil.c $(TRANSPORT_SRCS)
CAPTURE_HDRS := include/sensors/capture.h include/sensors/bh1750.h \
	include/sensors/seesaw_soil.h $(TRANSPORT_HDRS)
CAPTURE_LINUX_TEST_TARGET := $(BUILD_DIR)/test_capture_linux
CAPTURE_LINUX_SRCS := src/sensors/capture_linux.c src/sensors/i2c_linux.c \
	$(CAPTURE_SRCS)
CAPTURE_LINUX_HDRS := include/sensors/capture_linux.h \
	src/sensors/capture_linux_sys.h include/sensors/i2c_linux.h $(CAPTURE_HDRS)
CAPTURE_ARGS_TEST_TARGET := $(BUILD_DIR)/test_capture_args
CAPTURE_ARGS_SRCS := src/sensors/capture_args.c $(CAPTURE_SRCS)
CAPTURE_ARGS_HDRS := include/sensors/capture_args.h $(CAPTURE_HDRS)
CAPTURE_LOG_TEST_TARGET := $(BUILD_DIR)/test_capture_log
CAPTURE_LOG_SRCS := src/sensors/capture_log.c $(CAPTURE_SRCS)
CAPTURE_LOG_HDRS := include/sensors/capture_log.h $(CAPTURE_HDRS)

.PHONY: all test asan memcheck clean

all: $(TARGET) $(DIAG_TARGET)

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

$(BH1750_TEST_TARGET): tests/test_bh1750.c $(BH1750_SRCS) $(BH1750_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_bh1750.c $(BH1750_SRCS) -o $(BH1750_TEST_TARGET)

$(SOIL_TEST_TARGET): tests/test_seesaw_soil.c $(SOIL_SRCS) $(SOIL_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_seesaw_soil.c $(SOIL_SRCS) -o $(SOIL_TEST_TARGET)

# sensor-diag is built but never run by the tests: it opens /dev/i2c-N.
$(DIAG_TARGET): src/sensors/sensor_diag_main.c $(DIAG_SRCS) $(DIAG_HDRS) \
	src/sensors/i2c_linux.c $(LINUX_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) src/sensors/sensor_diag_main.c $(DIAG_SRCS) \
		src/sensors/i2c_linux.c -o $(DIAG_TARGET)

$(DIAG_TEST_TARGET): tests/test_sensor_diag.c $(DIAG_SRCS) $(DIAG_HDRS) \
	src/sensors/i2c_mock.c include/sensors/i2c_mock.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_sensor_diag.c $(DIAG_SRCS) \
		src/sensors/i2c_mock.c -o $(DIAG_TEST_TARGET)

# The capture core has no program yet; only its SIMULATED test is built.
$(CAPTURE_TEST_TARGET): tests/test_capture.c $(CAPTURE_SRCS) $(CAPTURE_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_capture.c $(CAPTURE_SRCS) -o $(CAPTURE_TEST_TARGET)

$(CAPTURE_LINUX_TEST_TARGET): tests/test_capture_linux.c $(CAPTURE_LINUX_SRCS) \
	$(CAPTURE_LINUX_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_capture_linux.c $(CAPTURE_LINUX_SRCS) \
		-o $(CAPTURE_LINUX_TEST_TARGET)

$(CAPTURE_ARGS_TEST_TARGET): tests/test_capture_args.c $(CAPTURE_ARGS_SRCS) \
	$(CAPTURE_ARGS_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_capture_args.c $(CAPTURE_ARGS_SRCS) \
		-o $(CAPTURE_ARGS_TEST_TARGET)

$(CAPTURE_LOG_TEST_TARGET): tests/test_capture_log.c $(CAPTURE_LOG_SRCS) \
	$(CAPTURE_LOG_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_capture_log.c $(CAPTURE_LOG_SRCS) \
		-o $(CAPTURE_LOG_TEST_TARGET)

test: $(TEST_TARGET) $(TRANSPORT_TEST_TARGET) $(LINUX_TEST_TARGET) \
	$(BH1750_TEST_TARGET) $(SOIL_TEST_TARGET) $(DIAG_TEST_TARGET) \
	$(CAPTURE_TEST_TARGET) $(CAPTURE_LINUX_TEST_TARGET) \
	$(CAPTURE_ARGS_TEST_TARGET) $(CAPTURE_LOG_TEST_TARGET)
	./$(TEST_TARGET)
	./$(TRANSPORT_TEST_TARGET)
	./$(LINUX_TEST_TARGET)
	./$(BH1750_TEST_TARGET)
	./$(SOIL_TEST_TARGET)
	./$(DIAG_TEST_TARGET)
	./$(CAPTURE_TEST_TARGET)
	./$(CAPTURE_LINUX_TEST_TARGET)
	./$(CAPTURE_ARGS_TEST_TARGET)
	./$(CAPTURE_LOG_TEST_TARGET)

ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -g

# asan: build and test in $(BUILD_DIR)/asan so normal binaries are untouched.
asan:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/asan CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" all test

memcheck: $(TEST_TARGET) $(TRANSPORT_TEST_TARGET) $(LINUX_TEST_TARGET) \
	$(BH1750_TEST_TARGET) $(SOIL_TEST_TARGET) $(DIAG_TEST_TARGET) \
	$(CAPTURE_TEST_TARGET) $(CAPTURE_LINUX_TEST_TARGET) \
	$(CAPTURE_ARGS_TEST_TARGET) $(CAPTURE_LOG_TEST_TARGET)
	@if command -v valgrind >/dev/null 2>&1; then \
		valgrind --leak-check=full --error-exitcode=1 ./$(TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(TRANSPORT_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(LINUX_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(BH1750_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(SOIL_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(DIAG_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(CAPTURE_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(CAPTURE_LINUX_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(CAPTURE_ARGS_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(CAPTURE_LOG_TEST_TARGET); \
	else \
		echo "valgrind is not installed"; exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)
