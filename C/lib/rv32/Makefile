CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -pedantic

all: test

rv32.o: rv32.c rv32.h
	$(CC) $(CFLAGS) -ffreestanding -c rv32.c -o $@

test_rv32: test_rv32.c rv32.o rv32.h
	$(CC) $(CFLAGS) test_rv32.c rv32.o -o $@

test: test_rv32
	./test_rv32

clean:
	rm -f rv32.o test_rv32

.PHONY: all test clean
