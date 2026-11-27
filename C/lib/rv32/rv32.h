/*
 * rv32.h - RV32IM interpreter core
 *
 * Portable C99, freestanding: needs only <stdint.h> and <stddef.h>
 * All state lives in rv32_t, so any number of cores can run side by side
 *
 * Memory model: one flat little-endian RAM window [ram_base, ram_base +
 * ram_size)
 * Any access outside it goes to optional MMIO callbacks
 * Misaligned data accesses are supported; instruction fetch must be 4-byte
 * aligned (no C extension)
 */

#ifndef RV32_H
#define RV32_H

#include <stddef.h>
#include <stdint.h>

enum rv32_status {
  RV32_OK = 0,           /* instruction retired normally                */
  RV32_ECALL,            /* ecall executed, pc already advanced         */
  RV32_EBREAK,           /* ebreak executed, pc already advanced        */
  RV32_ILLEGAL,          /* illegal instruction, pc unchanged           */
  RV32_FETCH_MISALIGNED, /* pc or jump/branch target not 4-aligned   */
  RV32_ACCESS_FAULT      /* load/store/fetch outside RAM and MMIO       */
};

/* Return 0 on success, nonzero if address is not handled */
typedef int (*rv32_mmio_read_fn)(void *user, uint32_t addr, unsigned size,
                                 uint32_t *value);
typedef int (*rv32_mmio_write_fn)(void *user, uint32_t addr, unsigned size,
                                  uint32_t value);

typedef struct rv32 {
  uint32_t x[32]; /* x[0] is forced to 0 after every step */
  uint32_t pc;

  uint8_t *ram;
  uint32_t ram_base;
  uint32_t ram_size;

  rv32_mmio_read_fn mmio_read;
  rv32_mmio_write_fn mmio_write;
  void *user;

  uint64_t instret;    /* Retired instruction count            */
  uint32_t fault_addr; /* Offending address on a fault/misalign */
} rv32_t;

/* Reset registers and counters; sets pc and sp (x2)
 * RAM is not touched */
void rv32_init(rv32_t *c, uint8_t *ram, uint32_t ram_base, uint32_t ram_size,
               uint32_t entry_pc, uint32_t stack_top);

/* Execute one instruction. */
enum rv32_status rv32_step(rv32_t *c);

/* Execute up to max_steps instructions (0 = unlimited)
 * Returns first non-OK status, or RV32_OK if budget ran out first */
enum rv32_status rv32_run(rv32_t *c, uint64_t max_steps);

#endif /* RV32_H */
