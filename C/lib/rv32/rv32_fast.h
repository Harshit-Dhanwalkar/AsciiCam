/*
 * rv32_fast.h - faster execution engine for RV32IM core
 *
 * Same machine, same results as rv32_run(), but instructions are decoded once
 * into a cache and dispatched with computed goto (GCC/Clang) or a switch (any
 * C99 compiler)
 * Anything unusual (MMIO, faults, ecall/ebreak, rare encodings) falls back to
 * rv32_step(), so behaviour stays identical
 *
 * Cache lives in caller-provided memory: no malloc, no libc
 *
 * Coherence: stores executed by CPU invalidate cache automatically
 * If anything else changes RAM (loading a new program, an MMIO callback doing
 * DMA, ...) call rv32_fast_flush() before running again
 */

#ifndef RV32_FAST_H
#define RV32_FAST_H

#include "rv32.h"

typedef struct rv32_dinst {
  uint8_t op; // Decoded opcode, 0 = not decoded yet
  uint8_t rd;
  uint8_t rs1;
  uint8_t rs2;
  int32_t imm;
} rv32_dinst_t; // 8 bytes: one entry per 4-byte RAM word

typedef struct rv32_fast {
  rv32_t *cpu;
  rv32_dinst_t *cache;
  uint32_t words; // Cache entries; code beyond them runs via rv32_step
} rv32_fast_t;

// Cache_words * 8 bytes of caller memory; ram_size/4 entries cover all RAM
void rv32_fast_init(rv32_fast_t *f, rv32_t *cpu, rv32_dinst_t *cache,
                    uint32_t cache_words);

// Drop every decoded instruction
void rv32_fast_flush(rv32_fast_t *f);

// Drop decoded instructions overlapping [addr, addr + len)
void rv32_fast_invalidate(rv32_fast_t *f, uint32_t addr, uint32_t len);

// Same contract as rv32_run()
enum rv32_status rv32_fast_run(rv32_fast_t *f, uint64_t max_steps);

#endif /* RV32_FAST_H */
