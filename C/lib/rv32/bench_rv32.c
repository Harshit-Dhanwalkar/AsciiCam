// bench_rv32.c - interpreter throughput: rv32_run() vs rv32_fast_run()
#include "rv32_enc.h"
#include "rv32_fast.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BASE 0x80000000u
#define RAMSZ 0x10000u

static uint8_t ram[RAMSZ];
static rv32_dinst_t cache[RAMSZ / 4];

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// 12-instruction mixed loop: load, add, xor, shifts, mul, store, branch
static unsigned build(uint32_t iters) {
  uint32_t p[32];
  unsigned n = 0, i, loop;
  n += rv32_li(p + n, 5, BASE + 0x8000); /* Data pointer */
  n += rv32_li(p + n, 10, iters);
  p[n++] = ADDI(8, 0, 1);  /* acc */
  p[n++] = ADDI(11, 0, 0); /* i   */
  loop = n;
  p[n++] = LW(13, 5, 0);
  p[n++] = ADD(8, 8, 13);
  p[n++] = enc_r(0x33, 4, 0, 14, 8, 11); /* xor x14,x8,x11 */
  p[n++] = enc_i(0x13, 1, 14, 14, 3);    /* slli x14,x14,3 */
  p[n++] = MUL(15, 14, 13);
  p[n++] = enc_i(0x13, 5, 15, 15, 5); /* srli x15,x15,5 */
  p[n++] = ADD(8, 8, 15);
  p[n++] = enc_i(0x13, 7, 16, 11, 0x3FC); /* andi x16,x11,0x3fc */
  p[n++] = ADD(16, 16, 5);
  p[n++] = SW(8, 16, 0);
  p[n++] = ADDI(11, 11, 1);
  p[n] = BNE(11, 10, ((int)loop - (int)n) * 4);
  n++;
  p[n++] = ECALL;
  memset(ram, 0, sizeof ram);
  for (i = 0; i < n; i++) {
    ram[i * 4] = (uint8_t)p[i];
    ram[i * 4 + 1] = (uint8_t)(p[i] >> 8);
    ram[i * 4 + 2] = (uint8_t)(p[i] >> 16);
    ram[i * 4 + 3] = (uint8_t)(p[i] >> 24);
  }
  return n;
}

int main(void) {
  const uint32_t iters = 20000000u;
  double best_slow = 1e9;
  double best_fast = 1e9;
  uint32_t res_slow = 0;
  uint32_t res_fast = 0;
  uint64_t insns = 0;
  int r;

  for (r = 0; r < 3; r++) {
    rv32_t c;
    rv32_fast_t f;
    double t;

    build(iters);
    rv32_init(&c, ram, BASE, RAMSZ, BASE, BASE + RAMSZ);
    t = now();
    rv32_run(&c, 0);
    t = now() - t;
    if (t < best_slow) {
      best_slow = t;
    }
    res_slow = c.x[8];
    insns = c.instret;

    build(iters);
    rv32_init(&c, ram, BASE, RAMSZ, BASE, BASE + RAMSZ);
    rv32_fast_init(&f, &c, cache, RAMSZ / 4);
    t = now();
    rv32_fast_run(&f, 0);
    t = now() - t;
    if (t < best_fast) {
      best_fast = t;
    }
    res_fast = c.x[8];
  }

  printf("instructions: %llu, results %s (0x%08x)\n", (unsigned long long)insns,
         res_slow == res_fast ? "match" : "DIFFER", res_slow);
  printf("rv32_run      : %.3f s  %7.1f MIPS\n", best_slow,
         (double)insns / best_slow / 1e6);
  printf("rv32_fast_run : %.3f s  %7.1f MIPS  (%.2fx)\n", best_fast,
         (double)insns / best_fast / 1e6, best_slow / best_fast);
  return res_slow == res_fast ? 0 : 1;
}
