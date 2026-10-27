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
RECORD_TEST_TARGET := $(BUILD_DIR)/test_storage_record
RECORD_SRCS := src/storage/record.c src/storage/crc32.c
RECORD_HDRS := include/storage/record.h include/storage/crc32.h

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

$(RECORD_TEST_TARGET): tests/test_storage_record.c $(RECORD_SRCS) $(RECORD_HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_storage_record.c $(RECORD_SRCS) -o $(RECORD_TEST_TARGET)

test: $(TEST_TARGET) $(TRANSPORT_TEST_TARGET) $(LINUX_TEST_TARGET) \
	$(BH1750_TEST_TARGET) $(SOIL_TEST_TARGET) $(DIAG_TEST_TARGET) \
	$(RECORD_TEST_TARGET)
	./$(TEST_TARGET)
	./$(TRANSPORT_TEST_TARGET)
	./$(LINUX_TEST_TARGET)
	./$(BH1750_TEST_TARGET)
	./$(SOIL_TEST_TARGET)
	./$(DIAG_TEST_TARGET)
	./$(RECORD_TEST_TARGET)

ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -g

# asan: build and test in $(BUILD_DIR)/asan so normal binaries are untouched.
asan:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/asan CFLAGS="$(CFLAGS) $(ASAN_FLAGS)" all test

memcheck: $(TEST_TARGET) $(TRANSPORT_TEST_TARGET) $(LINUX_TEST_TARGET) \
	$(BH1750_TEST_TARGET) $(SOIL_TEST_TARGET) $(DIAG_TEST_TARGET) \
	$(RECORD_TEST_TARGET)
	@if command -v valgrind >/dev/null 2>&1; then \
		valgrind --leak-check=full --error-exitcode=1 ./$(TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(TRANSPORT_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(LINUX_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(BH1750_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(SOIL_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(DIAG_TEST_TARGET) && \
		valgrind --leak-check=full --error-exitcode=1 ./$(RECORD_TEST_TARGET); \
	else \
		echo "valgrind is not installed"; exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)
