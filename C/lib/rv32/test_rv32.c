// test_rv32.c - self-checking tests for RV32IM core (hosted, uses libc)
#include "rv32.h"
#include <stdio.h>
#include <string.h>

// Tiny instruction encoders (reusable later by assembler)
static uint32_t enc_r(unsigned op, unsigned f3, unsigned f7, unsigned rd,
                      unsigned rs1, unsigned rs2) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t enc_i(unsigned op, unsigned f3, unsigned rd, unsigned rs1,
                      int imm) {
  return ((uint32_t)imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t enc_s(unsigned f3, unsigned rs1, unsigned rs2, int imm) {
  uint32_t i = (uint32_t)imm;
  return ((i >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
         ((i & 31u) << 7) | 0x23;
}
static uint32_t enc_b(unsigned f3, unsigned rs1, unsigned rs2, int imm) {
  uint32_t i = (uint32_t)imm;
  return (((i >> 12) & 1u) << 31) | (((i >> 5) & 0x3Fu) << 25) | (rs2 << 20) |
         (rs1 << 15) | (f3 << 12) | (((i >> 1) & 0xFu) << 8) |
         (((i >> 11) & 1u) << 7) | 0x63;
}
static uint32_t enc_u(unsigned op, unsigned rd, uint32_t imm20) {
  return (imm20 << 12) | (rd << 7) | op;
}
static uint32_t enc_j(unsigned rd, int imm) {
  uint32_t i = (uint32_t)imm;
  return (((i >> 20) & 1u) << 31) | (((i >> 1) & 0x3FFu) << 21) |
         (((i >> 11) & 1u) << 20) | (((i >> 12) & 0xFFu) << 12) | (rd << 7) |
         0x6F;
}

#define ADDI(rd, rs, i) enc_i(0x13, 0, rd, rs, i)
#define ADD(rd, a, b) enc_r(0x33, 0, 0x00, rd, a, b)
#define SUB(rd, a, b) enc_r(0x33, 0, 0x20, rd, a, b)
#define SLL(rd, a, b) enc_r(0x33, 1, 0x00, rd, a, b)
#define SLT(rd, a, b) enc_r(0x33, 2, 0x00, rd, a, b)
#define SLTU(rd, a, b) enc_r(0x33, 3, 0x00, rd, a, b)
#define SRL(rd, a, b) enc_r(0x33, 5, 0x00, rd, a, b)
#define SRA(rd, a, b) enc_r(0x33, 5, 0x20, rd, a, b)
#define MUL(rd, a, b) enc_r(0x33, 0, 1, rd, a, b)
#define MULH(rd, a, b) enc_r(0x33, 1, 1, rd, a, b)
#define MULHSU(rd, a, b) enc_r(0x33, 2, 1, rd, a, b)
#define MULHU(rd, a, b) enc_r(0x33, 3, 1, rd, a, b)
#define DIV(rd, a, b) enc_r(0x33, 4, 1, rd, a, b)
#define DIVU(rd, a, b) enc_r(0x33, 5, 1, rd, a, b)
#define REM(rd, a, b) enc_r(0x33, 6, 1, rd, a, b)
#define REMU(rd, a, b) enc_r(0x33, 7, 1, rd, a, b)
#define LUI(rd, i) enc_u(0x37, rd, i)
#define AUIPC(rd, i) enc_u(0x17, rd, i)
#define LB(rd, rs, i) enc_i(0x03, 0, rd, rs, i)
#define LH(rd, rs, i) enc_i(0x03, 1, rd, rs, i)
#define LW(rd, rs, i) enc_i(0x03, 2, rd, rs, i)
#define LBU(rd, rs, i) enc_i(0x03, 4, rd, rs, i)
#define LHU(rd, rs, i) enc_i(0x03, 5, rd, rs, i)
#define SB(rs2, rs1, i) enc_s(0, rs1, rs2, i)
#define SH(rs2, rs1, i) enc_s(1, rs1, rs2, i)
#define SW(rs2, rs1, i) enc_s(2, rs1, rs2, i)
#define BEQ(a, b, o) enc_b(0, a, b, o)
#define BNE(a, b, o) enc_b(1, a, b, o)
#define BLT(a, b, o) enc_b(4, a, b, o)
#define BGE(a, b, o) enc_b(5, a, b, o)
#define BLTU(a, b, o) enc_b(6, a, b, o)
#define BGEU(a, b, o) enc_b(7, a, b, o)
#define JAL(rd, o) enc_j(rd, o)
#define JALR(rd, rs, i) enc_i(0x67, 0, rd, rs, i)
#define ECALL 0x00000073u
#define EBREAK 0x00100073u

// Harness
#define BASE 0x80000000u
#define UART 0x10000000u
#define TIMER 0x10000100u

static uint8_t ram[4096];
static rv32_t cpu;
static int checks, failures;
static char uart_out[64];
static unsigned uart_len;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)
#define EQ(a, b)                                                               \
  do {                                                                         \
    uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b);                           \
    checks++;                                                                  \
    if (_a != _b) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = 0x%08x, expected 0x%08x\n", __FILE__, __LINE__, \
             #a, _a, _b);                                                      \
    }                                                                          \
  } while (0)

static int mmio_rd(void *u, uint32_t a, unsigned sz, uint32_t *v) {
  (void)u;
  (void)sz;
  if (a == TIMER) {
    *v = 12345;
    return 0;
  }
  return -1;
}
static int mmio_wr(void *u, uint32_t a, unsigned sz, uint32_t v) {
  (void)u;
  (void)sz;
  if (a == UART) {
    if (uart_len < sizeof uart_out) {
      uart_out[uart_len++] = (char)v;
    }
    return 0;
  }
  return -1;
}

static void load(const uint32_t *prog, unsigned n) {
  unsigned i;
  memset(ram, 0, sizeof ram);
  for (i = 0; i < n; i++) {
    ram[i * 4 + 0] = (uint8_t)prog[i];
    ram[i * 4 + 1] = (uint8_t)(prog[i] >> 8);
    ram[i * 4 + 2] = (uint8_t)(prog[i] >> 16);
    ram[i * 4 + 3] = (uint8_t)(prog[i] >> 24);
  }
  rv32_init(&cpu, ram, BASE, sizeof ram, BASE, BASE + sizeof ram);
  cpu.mmio_read = mmio_rd;
  cpu.mmio_write = mmio_wr;
  uart_len = 0;
}
#define RUN(p) (load(p, sizeof(p) / sizeof((p)[0])), rv32_run(&cpu, 100000))
#define X(n) (cpu.x[n])

// Load a 32-bit constant into rd (LUI + ADDI with carry fix-up)
static unsigned li(uint32_t *p, unsigned rd, uint32_t v) {
  uint32_t lo = v & 0xFFFu;
  uint32_t hi = v >> 12;
  if (lo & 0x800u) {
    hi = (hi + 1) & 0xFFFFFu;
  }
  p[0] = LUI(rd, hi);
  p[1] = ADDI(rd, rd, (int)(lo & 0x800u ? lo - 0x1000u : lo));
  return 2;
}

static void test_alu(void) {
  uint32_t p[32];
  unsigned n = 0;
  n += li(p + n, 1, 0x80000000u);
  p[n++] = ADDI(2, 0, 3);
  p[n++] = ADDI(0, 0, 77);                   // writes to x0 must be ignored
  p[n++] = SUB(3, 2, 1);                     // 3 - 0x80000000
  p[n++] = SLL(4, 2, 2);                     // 3 << 3 = 24
  p[n++] = SRL(5, 1, 2);                     // 0x80000000 >> 3
  p[n++] = SRA(6, 1, 2);                     // arithmetic
  p[n++] = SLT(7, 1, 2);                     // signed: INT_MIN < 3
  p[n++] = SLTU(8, 1, 2);                    // unsigned: no
  p[n++] = enc_i(0x13, 5, 9, 1, 0x400 | 31); // SRAI x9,x1,31
  p[n++] = ECALL;
  EQ(RUN(p), RV32_ECALL);
  EQ(X(0), 0);
  EQ(X(3), 0x80000003u);
  EQ(X(4), 24);
  EQ(X(5), 0x10000000u);
  EQ(X(6), 0xF0000000u);
  EQ(X(7), 1);
  EQ(X(8), 0);
  EQ(X(9), 0xFFFFFFFFu);
}

static void test_muldiv(void) {
  uint32_t p[48];
  unsigned n = 0;
  n += li(p + n, 1, 0xFFFFFFFFu); // -1 / UINT_MAX
  n += li(p + n, 2, 0x80000000u); // INT_MIN
  p[n++] = ADDI(3, 0, 7);
  p[n++] = MUL(10, 1, 3);    // -7
  p[n++] = MULH(11, 1, 1);   // (-1*-1)>>32 = 0
  p[n++] = MULH(12, 2, 2);   // 2^62 >> 32 = 0x40000000
  p[n++] = MULHSU(13, 1, 1); // -1 * UINT_MAX = -(2^32-1) -> hi = -1
  p[n++] = MULHU(14, 1, 1);  // 0xFFFFFFFE
  p[n++] = DIV(15, 2, 1);    // overflow -> INT_MIN
  p[n++] = REM(16, 2, 1);    // overflow -> 0
  p[n++] = DIV(17, 3, 0);    // /0 -> -1
  p[n++] = REM(18, 3, 0);    // %0 -> dividend
  p[n++] = DIVU(19, 3, 0);   // /0 -> UINT_MAX
  p[n++] = REMU(20, 3, 0);   // %0 -> dividend
  p[n++] = DIV(21, 1, 3);    // -1/7 = 0
  p[n++] = REM(22, 1, 3);    // -1%7 = -1
  p[n++] = DIVU(23, 1, 3);   // 0xFFFFFFFF/7
  p[n++] = REMU(24, 1, 3);
  p[n++] = ECALL;
  EQ(RUN(p), RV32_ECALL);
  EQ(X(10), (uint32_t)-7);
  EQ(X(11), 0);
  EQ(X(12), 0x40000000u);
  EQ(X(13), 0xFFFFFFFFu);
  EQ(X(14), 0xFFFFFFFEu);
  EQ(X(15), 0x80000000u);
  EQ(X(16), 0);
  EQ(X(17), 0xFFFFFFFFu);
  EQ(X(18), 7);
  EQ(X(19), 0xFFFFFFFFu);
  EQ(X(20), 7);
  EQ(X(21), 0);
  EQ(X(22), 0xFFFFFFFFu);
  EQ(X(23), 0xFFFFFFFFu / 7u);
  EQ(X(24), 0xFFFFFFFFu % 7u);
}

static void test_loop_and_branches(void) {
  // sum 1..10
  uint32_t p[] = {ADDI(10, 0, 0),  // a0 = 0
                  ADDI(11, 0, 1),  // i  = 1
                  ADDI(12, 0, 11), // lim = 11
                  ADD(10, 10, 11), // loop: a0 += i
                  ADDI(11, 11, 1),
                  BLT(11, 12, -8), // if i < lim goto loop
                  ECALL};
  EQ(RUN(p), RV32_ECALL);
  EQ(X(10), 55);
  EQ(cpu.instret, 3 + 10 * 3 + 1);
}

static void test_branch_kinds(void) {
  // x1=-1, x2=1
  // Each taken branch skips an "ADDI x10,x10,1" marker
  uint32_t p[] = {
      ADDI(1, 0, -1), ADDI(2, 0, 1),     ADDI(10, 0, 0),
      BEQ(1, 1, 8),   ADDI(10, 10, 1),   // Taken
      BNE(1, 2, 8),   ADDI(10, 10, 1),   // Taken
      BLT(1, 2, 8),   ADDI(10, 10, 1),   // Taken   (-1 < 1 signed)
      BGE(2, 1, 8),   ADDI(10, 10, 1),   // Taken
      BLTU(2, 1, 8),  ADDI(10, 10, 1),   // Taken   (1 < 0xFFFFFFFF)
      BGEU(1, 2, 8),  ADDI(10, 10, 1),   // Taken
      BLT(2, 1, 8),   ADDI(10, 10, 100), // Not taken: executes marker
      ECALL};
  EQ(RUN(p), RV32_ECALL);
  EQ(X(10), 100);
}

static void test_jumps(void) {
  uint32_t p[] = {
      JAL(1, 12),      // 0x00: ra = 4, jump to 0x0C
      ADDI(10, 0, 99), // 0x04: return lands here
      ECALL,           // 0x08
      ADDI(11, 0, 5),  // 0x0C: callee
      JALR(0, 1, 0),   // 0x10: ret -> 0x04
  };
  EQ(RUN(p), RV32_ECALL);
  EQ(X(1), BASE + 4);
  EQ(X(10), 99);
  EQ(X(11), 5);
  { // rd == rs1 for JALR; AUIPC
    uint32_t q[] = {
        AUIPC(5, 0),    // x5 = pc = BASE
        ADDI(5, 5, 12), // x5 = BASE + 12
        JALR(5, 5, 0),  // jump via x5, link -> x5
        EBREAK,         // 0x0C: jump target
        ECALL           // 0x10
    };
    // JALR target is BASE+12 (EBREAK); link must land in x5
    EQ(RUN(q), RV32_EBREAK);
    EQ(X(5), BASE + 12);
  }
}

static void test_memory(void) {
  uint32_t p[32];
  unsigned n = 0;
  n += li(p + n, 1, BASE + 0x800); // Data pointer
  n += li(p + n, 2, 0x8899AABBu);
  p[n++] = SW(2, 1, 0);
  p[n++] = LB(10, 1, 0); // 0xBB sign-ext
  p[n++] = LBU(11, 1, 0);
  p[n++] = LH(12, 1, 0); // 0xAABB sign-ext
  p[n++] = LHU(13, 1, 0);
  p[n++] = LW(14, 1, 0);
  p[n++] = LW(15, 1, 1); // Misaligned word read: bytes 1..4
  p[n++] = SB(2, 1, 8);
  p[n++] = SH(2, 1, 13); // Misaligned halfword store
  p[n++] = LW(16, 1, 8);
  p[n++] = ECALL;
  EQ(RUN(p), RV32_ECALL);
  EQ(X(10), 0xFFFFFFBBu);
  EQ(X(11), 0xBB);
  EQ(X(12), 0xFFFFAABBu);
  EQ(X(13), 0xAABB);
  EQ(X(14), 0x8899AABBu);
  EQ(X(15), 0x008899AAu);
  EQ(X(16), 0xBB);
  EQ(ram[0x800 + 13], 0xBB);
  EQ(ram[0x800 + 14], 0xAA);
}

static void test_mmio(void) {
  uint32_t p[16];
  unsigned n = 0;
  n += li(p + n, 1, UART);
  n += li(p + n, 3, TIMER);
  p[n++] = ADDI(2, 0, 'H');
  p[n++] = SB(2, 1, 0);
  p[n++] = ADDI(2, 0, 'i');
  p[n++] = SB(2, 1, 0);
  p[n++] = LW(10, 3, 0);
  p[n++] = ECALL;
  EQ(RUN(p), RV32_ECALL);
  CHECK(uart_len == 2 && uart_out[0] == 'H' && uart_out[1] == 'i');
  EQ(X(10), 12345);
}

static void test_faults(void) {
  uint32_t bad[] = {0x00000000u};
  uint32_t mis[] = {JAL(0, 6)};
  uint32_t oob[16];
  unsigned n = 0;
  uint32_t ret[] = {JALR(0, 1, 2)};
  EQ(RUN(bad), RV32_ILLEGAL);
  EQ(cpu.pc, BASE);
  EQ(RUN(mis), RV32_FETCH_MISALIGNED);
  EQ(cpu.pc, BASE);
  n += li(oob, 1, 0x40000000u);
  oob[n++] = LW(2, 1, 0);
  EQ(RUN(oob), RV32_ACCESS_FAULT);
  EQ(cpu.fault_addr, 0x40000000u);
  EQ(RUN(ret), RV32_FETCH_MISALIGNED); // x1=0, +2 -> Misaligned target
  EQ(RUN((uint32_t[]){EBREAK}), RV32_EBREAK);
  // Step budget
  {
    uint32_t spin[] = {JAL(0, 0)};
    load(spin, 1);
    EQ(rv32_run(&cpu, 1000), RV32_OK);
    EQ(cpu.instret, 1000);
  }
}

int main(void) {
  test_alu();
  test_muldiv();
  test_loop_and_branches();
  test_branch_kinds();
  test_jumps();
  test_memory();
  test_mmio();
  test_faults();

  printf("%d checks, %d failures\n", checks, failures);

  return failures ? 1 : 0;
}
