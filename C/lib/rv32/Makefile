CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic

BUILD_DIR := build

all: test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/rv32.o: rv32.c rv32.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -ffreestanding -c rv32.c -o $@

$(BUILD_DIR)/rv32_fast.o: rv32_fast.c rv32_fast.h rv32.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -ffreestanding -c rv32_fast.c -o $@

$(BUILD_DIR)/rv32_fast_sw.o: rv32_fast.c rv32_fast.h rv32.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -ffreestanding -DRV32_NO_COMPUTED_GOTO -c rv32_fast.c -o $@

$(BUILD_DIR)/test_rv32: test_rv32.c $(BUILD_DIR)/rv32.o rv32.h rv32_enc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) test_rv32.c $(BUILD_DIR)/rv32.o -o $@

$(BUILD_DIR)/test_fast: test_fast.c $(BUILD_DIR)/rv32.o $(BUILD_DIR)/rv32_fast.o rv32_fast.h rv32_enc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) test_fast.c $(BUILD_DIR)/rv32.o $(BUILD_DIR)/rv32_fast.o -o $@

$(BUILD_DIR)/test_fast_sw: test_fast.c $(BUILD_DIR)/rv32.o $(BUILD_DIR)/rv32_fast_sw.o rv32_fast.h rv32_enc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) test_fast.c $(BUILD_DIR)/rv32.o $(BUILD_DIR)/rv32_fast_sw.o -o $@

$(BUILD_DIR)/bench_rv32: bench_rv32.c $(BUILD_DIR)/rv32.o $(BUILD_DIR)/rv32_fast.o rv32_fast.h rv32_enc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=199309L bench_rv32.c $(BUILD_DIR)/rv32.o $(BUILD_DIR)/rv32_fast.o -o $@

test: $(BUILD_DIR)/test_rv32 $(BUILD_DIR)/test_fast $(BUILD_DIR)/test_fast_sw
	./$(BUILD_DIR)/test_rv32
	./$(BUILD_DIR)/test_fast
	./$(BUILD_DIR)/test_fast_sw

bench: $(BUILD_DIR)/bench_rv32
	./$(BUILD_DIR)/bench_rv32

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test bench clean
