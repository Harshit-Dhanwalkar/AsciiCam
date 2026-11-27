/*
 * test_fast.c - differential test: rv32_fast_run() must match rv32_run()
 * exactly (registers, pc, instret, fault info, RAM) on random programs,
 * including self-modifying code, faults and split run budgets
 */

#include "rv32_enc.h"
#include "rv32_fast.h"
#include <stdio.h>
#include <string.h>

#define BASE 0x80000000u
#define RAMSZ 1024u

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return (uint32_t)(rng_state >> 16);
}

static int mmio_rd(void *u, uint32_t a, unsigned sz, uint32_t *v) {
  (void)u;
  (void)sz;
  if ((a & 0xFFFFFF00u) == 0x10000000u) {
    *v = a * 2654435761u;
    return 0;
  }
  return -1;
}
static int mmio_wr(void *u, uint32_t a, unsigned sz, uint32_t v) {
  (void)u;
  (void)sz;
  (void)v;
  return (a & 0xFFFFFF00u) == 0x10000000u ? 0 : -1;
}

static uint32_t rand_inst(void) {
  unsigned rd = 3 + rnd() % 5, rs1 = 1 + rnd() % 7,
           rs2 = rnd() % 8;          /* rd avoids x1,x2 (pointers) */
  int imm = (int)(rnd() % 160) - 32; /* Small: keeps addresses near pointers */
  int boff = ((int)(rnd() % 17) - 8) * 4 + ((rnd() % 40) == 0 ? 2 : 0);
  switch (rnd() % 30) {
  case 0:
    return LUI(rd, rnd() & 0xFFFFFu);
  case 1:
    return AUIPC(rd, rnd() & 0xFFFFFu);
  case 2:
    return JAL(rd, boff);
  case 3:
    return JALR(rd, rs1, imm & ~3);
  case 4:
    return BEQ(rs1, rs2, boff);
  case 5:
    return BNE(rs1, rs2, boff);
  case 6:
    return BLT(rs1, rs2, boff);
  case 7:
    return BGE(rs1, rs2, boff);
  case 8:
    return BLTU(rs1, rs2, boff);
  case 9:
    return BGEU(rs1, rs2, boff);
  case 10:
    return LB(rd, 1 + rnd() % 2, imm);
  case 11:
    return LH(rd, rs1, imm);
  case 12:
    return LW(rd, 1 + rnd() % 2, imm);
  case 13:
    return LBU(rd, rs1, imm);
  case 14:
    return LHU(rd, rs1, imm);
  case 15:
    return SB(rs2, 1 + rnd() % 2, imm);
  case 16:
    return SH(rs2, rs1, imm);
  case 17:
    return SW(rs2, 1 + rnd() % 2, imm);
  case 18:
    return enc_i(
        0x13,
        rnd() % 8 == 1 ? 1 : (unsigned[]){0, 2, 3, 4, 6, 7, 0, 4}[rnd() % 8],
        rd, rs1, imm & 31);
  case 19:
    return enc_i(0x13, 5, rd, rs1,
                 (int)((rnd() & 1 ? 0x400 : 0) | (rnd() & 31)));
  case 20:
    return enc_i(0x13, (unsigned[]){0, 2, 3, 4, 6, 7}[rnd() % 6], rd, rs1, imm);
  case 21:
    return ADD(rd, rs1, rs2);
  case 22:
    return SUB(rd, rs1, rs2);
  case 23:
    return enc_r(0x33, (unsigned[]){1, 2, 3, 4, 6, 7}[rnd() % 6], 0x00, rd, rs1,
                 rs2);
  case 24:
    return rnd() & 1 ? SRL(rd, rs1, rs2) : SRA(rd, rs1, rs2);
  case 25:
  case 26:
    return enc_r(0x33, rnd() % 8, 0x01, rd, rs1, rs2); /* All M ops */
  case 27:
    return rnd() % 4 ? 0x0000000Fu : (rnd() & 1 ? ECALL : EBREAK);
  case 28:
    return rnd() % 8 ? ADDI(rd, rs1, imm) : rnd(); /* Rare garbage */
  default:
    return rnd() % 3 ? ADDI(rd, rs1, imm) : MUL(rd, rs1, rs2);
  }
}

static void setup(rv32_t *c, uint8_t *ram, const uint32_t *prog,
                  const uint32_t *regs) {
  unsigned i;
  for (i = 0; i < RAMSZ / 4; i++) {
    uint32_t w = i < 64 ? prog[i] : rnd();
    ram[i * 4] = (uint8_t)w;
    ram[i * 4 + 1] = (uint8_t)(w >> 8);
    ram[i * 4 + 2] = (uint8_t)(w >> 16);
    ram[i * 4 + 3] = (uint8_t)(w >> 24);
  }
  rv32_init(c, ram, BASE, RAMSZ, BASE, 0);
  for (i = 1; i < 32; i++) {
    c->x[i] = regs[i];
  }
  c->mmio_read = mmio_rd;
  c->mmio_write = mmio_wr;
}

static int same(const rv32_t *a, const rv32_t *b, const uint8_t *ra,
                const uint8_t *rb) {
  return !memcmp(a->x, b->x, sizeof a->x) && a->pc == b->pc &&
         a->instret == b->instret && a->fault_addr == b->fault_addr &&
         !memcmp(ra, rb, RAMSZ);
}

int main(void) {
  static uint8_t ram_a[RAMSZ], ram_b[RAMSZ];
  static rv32_dinst_t cache[RAMSZ / 4];
  unsigned iter;
  unsigned bad = 0;
  unsigned ok = 0;
  uint64_t total_steps = 0;
  unsigned status_hist[8] = {0};

  for (iter = 0; iter < 200000; iter++) {
    uint32_t prog[64], regs[32];
    rv32_t a;
    rv32_t b;
    rv32_fast_t f;
    unsigned i;
    uint64_t n1 = 1 + rnd() % 200;
    uint64_t n2 = rnd() % 200;
    enum rv32_status sa;
    enum rv32_status sb;

    for (i = 0; i < 64; i++) {
      prog[i] = rand_inst();
    }
    for (i = 1; i < 32; i++) {
      uint32_t r = rnd();
      regs[i] = (i <= 2 || (r & 3)) ? BASE + 256 + (r >> 4) % (RAMSZ - 300)
                : (r & 4)           ? 0x10000000u + (r >> 8) % 300
                                    : rnd();
    }
    setup(&a, ram_a, prog, regs);
    memcpy(ram_b, ram_a, RAMSZ); // Identical RAM image
    rv32_init(&b, ram_b, BASE, RAMSZ, BASE, 0);
    for (i = 1; i < 32; i++) {
      b.x[i] = regs[i];
    }
    b.mmio_read = mmio_rd;
    b.mmio_write = mmio_wr;

    sa = rv32_run(&a, n1);
    if (sa == RV32_OK && n2) {
      sa = rv32_run(&a, n2);
    }

    rv32_fast_init(&f, &b, cache, RAMSZ / 4);
    sb = rv32_fast_run(&f, n1);
    if (sb == RV32_OK && n2) {
      sb = rv32_fast_run(&f, n2); // Resume with a warm cache
    }

    total_steps += a.instret;
    status_hist[sa]++;
    if (sa != sb || !same(&a, &b, ram_a, ram_b)) {
      if (bad++ < 5) {
        printf("MISMATCH iter %u: status %d vs %d, pc %08x vs %08x, instret "
               "%llu vs %llu\n",
               iter, (int)sa, (int)sb, a.pc, b.pc,
               (unsigned long long)a.instret, (unsigned long long)b.instret);
      }
    } else
      ok++;
  }

  printf("%u ok, %u mismatches, %llu instructions compared\n", ok, bad,
         (unsigned long long)total_steps);
  printf("final status mix: ok=%u ecall=%u ebreak=%u illegal=%u fetch=%u "
         "access=%u\n",
         status_hist[0], status_hist[1], status_hist[2], status_hist[3],
         status_hist[4], status_hist[5]);

  return bad ? 1 : 0;
}
